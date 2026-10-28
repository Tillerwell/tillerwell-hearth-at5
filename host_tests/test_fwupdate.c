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
    printf("%s: %d checks, %d failures\n", failures ? "FAILED" : "OK", checks, failures);
    return failures != 0;
}
