// The remote firmware update's request check (docs/API_SPEC.md §9.7).   make -C host_tests fwupdate
#include <stdio.h>
#include <string.h>

#include "at_fwupdate.h"

static int failures, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { failures++; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

#define SHA "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"
#define URL "https://github.com/Tillerwell/tillerwell-hearth-at5/releases/download/1.0032/hearth-1.0032.bin"

static const char *check(const char *json, const char *running, uint32_t max, at_fwupdate_req_t *r)
{
    return at_fwupdate_parse(json, running, max, r);
}

static void expect(const char *name, const char *json, const char *running, uint32_t max, const char *want)
{
    at_fwupdate_req_t r;
    const char *got = check(json, running, max, &r);
    CHECK((got == NULL && want == NULL) || (got && want && strcmp(got, want) == 0), "%s: got %s, want %s", name,
          got ? got : "ok", want ? want : "ok");
}

int main(void)
{
    const char *good = "{\"cmd\":\"ota\",\"version\":\"1.0032\",\"url\":\"" URL "\",\"sha256\":\"" SHA "\",\"size\":2400000}";
    at_fwupdate_req_t r;
    CHECK(check(good, "1.0031", 3600000, &r) == NULL, "valid request");
    CHECK(!strcmp(r.version, "1.0032") && !strcmp(r.sha256, SHA) && r.size == 2400000 && !strcmp(r.url, URL), "fields kept");
    expect("same version (reinstall)", good, "1.0032", 3600000, NULL);
    expect("older than running", good, "1.0033", 3600000, "older");
    expect("running version not RoamOS", good, "dev-build", 3600000, NULL);
    expect("too big for the partition", good, "1.0031", 2000000, "too_big");

    expect("not json", "nope", "1.0031", 3600000, "malformed");
    expect("no version", "{\"cmd\":\"ota\",\"url\":\"" URL "\",\"sha256\":\"" SHA "\",\"size\":2400000}", "1.0031", 3600000, "malformed");
    expect("version not RoamOS", "{\"version\":\"1.2\",\"url\":\"" URL "\",\"sha256\":\"" SHA "\",\"size\":2400000}", "1.0031", 3600000, "malformed");
    expect("short hash", "{\"version\":\"1.0032\",\"url\":\"" URL "\",\"sha256\":\"abcd\",\"size\":2400000}", "1.0031", 3600000, "malformed");
    expect("upper-case hash", "{\"version\":\"1.0032\",\"url\":\"" URL "\",\"sha256\":\"0123456789ABCDEF0123456789abcdef0123456789abcdef0123456789abcdef\",\"size\":2400000}", "1.0031", 3600000, "malformed");
    expect("size is text", "{\"version\":\"1.0032\",\"url\":\"" URL "\",\"sha256\":\"" SHA "\",\"size\":\"2400000\"}", "1.0031", 3600000, "malformed");
    expect("tiny size", "{\"version\":\"1.0032\",\"url\":\"" URL "\",\"sha256\":\"" SHA "\",\"size\":10}", "1.0031", 3600000, "malformed");

    const char *urls[] = {
        "http://github.com/Tillerwell/tillerwell-hearth-at5/releases/download/1.0032/hearth-1.0032.bin",
        "https://evil.example/Tillerwell/tillerwell-hearth-at5/releases/download/1.0032/hearth-1.0032.bin",
        "https://github.com.evil.example/Tillerwell/tillerwell-hearth-at5/releases/download/1.0032/hearth-1.0032.bin",
        "https://github.com/Tillerwell/other/releases/download/1.0032/hearth-1.0032.bin",
        "https://github.com/Tillerwell/tillerwell-hearth-at5/releases/download/1.0032/hearth-1.0033.bin",
        "https://github.com/Tillerwell/tillerwell-hearth-at5/releases/download/1.0032/hearth-1.0032.bin?x=1",
        "https://github.com/Tillerwell/tillerwell-hearth-at5/releases/download/../../x/hearth-1.0032.bin",
        "https://github.com/Tillerwell/tillerwell-hearth-at5/releases/download/a/b/hearth-1.0032.bin",
        "https://github.com/Tillerwell/tillerwell-hearth-at5/releases/download/1.0032/hearth-1.0032.bin#x",
        "https://github.com/Tillerwell/tillerwell-hearth-at5/releases/download//hearth-1.0032.bin",
    };
    for (size_t i = 0; i < sizeof(urls) / sizeof(urls[0]); i++) {
        char json[600];
        snprintf(json, sizeof(json), "{\"version\":\"1.0032\",\"url\":\"%s\",\"sha256\":\"" SHA "\",\"size\":2400000}", urls[i]);
        expect(urls[i], json, "1.0031", 3600000, "bad_url");
    }

    // ---- automatic updates: when it is safe to install
    const int64_t now = 1000000;
    CHECK(at_autoupdate_idle(true, true, false, false, 0, now), "off, no timers: idle");
    CHECK(at_autoupdate_idle(true, true, false, true, now + AT_AUTOUPDATE_QUIET_S + 1, now), "next timer just beyond 30 min: idle");
    CHECK(!at_autoupdate_idle(true, true, false, true, now + AT_AUTOUPDATE_QUIET_S, now), "next timer at exactly 30 min: not idle");
    CHECK(!at_autoupdate_idle(true, true, false, true, now + 60, now), "timer in a minute: not idle");
    CHECK(!at_autoupdate_idle(true, true, false, true, now - 5, now), "timer start just passed: not idle");
    CHECK(!at_autoupdate_idle(true, true, true, false, 0, now), "a timer is running: not idle");
    CHECK(!at_autoupdate_idle(true, false, false, false, 0, now), "heater on: not idle");
    CHECK(!at_autoupdate_idle(false, true, false, false, 0, now), "clock not set: not idle");

    // ---- automatic updates: reading GitHub's latest release
    const char *rel =
        "{\"tag_name\":\"1.0033\",\"body\":\"notes\",\"assets\":[{\"name\":\"notes.txt\",\"size\":5},"
        "{\"name\":\"hearth-1.0033.bin\",\"size\":2400000,\"digest\":\"sha256:" SHA "\","
        "\"browser_download_url\":\"https://github.com/Tillerwell/tillerwell-hearth-at5/releases/download/1.0033/hearth-1.0033.bin\"}]}";
    char cmd[400];
    CHECK(at_autoupdate_pick(rel, "1.0032", cmd, sizeof(cmd)) == 1, "newer release found");
    at_fwupdate_req_t q;
    CHECK(at_fwupdate_parse(cmd, "1.0032", 3600000, &q) == NULL && !strcmp(q.version, "1.0033") && !strcmp(q.sha256, SHA) && q.size == 2400000,
          "the picked command passes the request check: %s", cmd);
    CHECK(at_autoupdate_pick(rel, "1.0033", cmd, sizeof(cmd)) == 0, "already on it");
    CHECK(at_autoupdate_pick(rel, "1.0034", cmd, sizeof(cmd)) == 0, "running newer than the release");
    CHECK(at_autoupdate_pick(rel, "dev-build", cmd, sizeof(cmd)) == 1, "running version not RoamOS is offered it");
    CHECK(at_autoupdate_pick(rel, "1.0032", cmd, 40) == -1, "buffer too small");
    CHECK(at_autoupdate_pick("nope", "1.0032", cmd, sizeof(cmd)) == -1, "not json");
    CHECK(at_autoupdate_pick("{\"assets\":[{\"name\":\"hearth-1.0033.bin\",\"size\":2400000,\"browser_download_url\":\"x\"}]}", "1.0032", cmd, sizeof(cmd)) == -1,
          "no checksum listed");
    CHECK(at_autoupdate_pick("{\"assets\":[]}", "1.0032", cmd, sizeof(cmd)) == -1, "no image");
    printf("%s: %d checks, %d failures\n", failures ? "FAILED" : "OK", checks, failures);
    return failures != 0;
}
