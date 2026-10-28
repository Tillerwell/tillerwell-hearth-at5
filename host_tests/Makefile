# Host-side tests for the protocol, bridge core and thermostat logic.
#   make -C host_tests          build and run
#   make -C host_tests vectors BASE=/path/to/autoterm-heater-control/autoterm
#   make -C host_tests auth           signing spec (docs/API_SPEC.md) against docs/auth-vectors.json,
#                                     using ESP-IDF's own mbedTLS and cJSON (IDF_PATH, default ~/esp/esp-idf)
#   make -C host_tests auth-vectors   regenerate the vectors (needs Python 'cryptography')
CC ?= cc
CFLAGS = -std=c11 -Wall -Wextra -Wno-unused-parameter -g -fsanitize=address,undefined -fno-omit-frame-pointer \
         -I../components/at_proto/include -I../components/at_core/include
SRCS = test_main.c ../components/at_proto/at_proto.c ../components/at_core/at_core.c ../components/at_core/at_ctl.c ../components/at_core/at_fuel.c ../components/at_core/at_timers.c ../components/at_core/at_tz.c ../components/at_core/at_version.c

test: test_main
	./test_main

test_main: $(SRCS) vectors.h
	$(CC) $(CFLAGS) $(SRCS) -o $@ -lm

vectors:
	python3 gen_vectors.py $(BASE) > vectors.h

IDF_PATH ?= $(HOME)/esp/esp-idf
MBEDTLS = build/mbedtls
CJSON = $(IDF_PATH)/components/json/cJSON
# -Wno-deprecated-declarations: IDF's bundled cJSON uses sprintf.
AUTH_CFLAGS = $(CFLAGS) -Wno-deprecated-declarations -I../components/at_auth/include -I$(MBEDTLS)/include -I$(CJSON)

auth: test_auth
	./test_auth

test_auth: test_auth.c ../components/at_auth/at_auth.c auth_vectors.h $(MBEDTLS)/library/libmbedcrypto.a
	$(CC) $(AUTH_CFLAGS) test_auth.c ../components/at_auth/at_auth.c $(CJSON)/cJSON.c $(MBEDTLS)/library/libmbedcrypto.a -o $@

# A private copy, so building it never touches the IDF tree.
$(MBEDTLS)/library/libmbedcrypto.a:
	rm -rf $(MBEDTLS) && mkdir -p build && cp -R $(IDF_PATH)/components/mbedtls/mbedtls $(MBEDTLS)
	$(MAKE) -C $(MBEDTLS) -j8 lib >/dev/null

ACCESS_CFLAGS = $(AUTH_CFLAGS) -I../components/at_access/include

access: test_access
	./test_access

test_access: test_access.c ../components/at_access/at_access.c ../components/at_auth/at_auth.c auth_vectors.h $(MBEDTLS)/library/libmbedcrypto.a
	$(CC) $(ACCESS_CFLAGS) test_access.c ../components/at_access/at_access.c ../components/at_auth/at_auth.c $(CJSON)/cJSON.c $(MBEDTLS)/library/libmbedcrypto.a -o $@

cloudlink: test_cloudlink
	./test_cloudlink

test_cloudlink: test_cloudlink.c ../components/at_cloudlink/at_cloudlink_policy.c
	$(CC) $(CFLAGS) -I../components/at_cloudlink/include test_cloudlink.c ../components/at_cloudlink/at_cloudlink_policy.c -o $@

auth-vectors:
	cd .. && python3 host_tests/gen_auth_vectors.py

clean:
	rm -f test_main test_auth test_access test_cloudlink
	rm -rf build
