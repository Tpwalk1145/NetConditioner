#include "test_harness.h"

#include "update/coordinator.h"
#include "core/str.h"
#include "update/selfupdate.h"
#include "update/update.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *valid_manifest =
    "{\"schemaVersion\":2,\"version\":\"9.9.9\",\"publishedAt\":\"2026-01-01T00:00:00Z\","
    "\"notes\":\"notes\",\"pageUrl\":\"https://example.com/\","
    "\"installer\":{\"url\":\"https://example.com/app.exe\","
    "\"sha256\":\"AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA\","
    "\"size\":7}}";

void test_update_run(void) {
    char sha[65];
    NcErr error;
    NcUpdateManifest manifest;
    NcVersion version;

    nc_sha256_hex("", 0, sha);
    CHECK_STR(sha, "E3B0C44298FC1C149AFBF4C8996FB92427AE41E4649B934CA495991B7852B855");
    nc_sha256_hex("abc", 3, sha);
    CHECK_STR(sha, "BA7816BF8F01CFEA414140DE5DAE2223B00361A396177A9CB410FF61F20015AD");

    CHECK(nc_update_parse_manifest(valid_manifest, strlen(valid_manifest), &manifest, &version,
                                   &error));
    CHECK_INT(manifest.schema_version, 2);
    CHECK_STR(manifest.version, "9.9.9");
    CHECK_STR(manifest.installer.url, "https://example.com/app.exe");
    CHECK_INT(manifest.installer.size, 7);
    CHECK_INT(version.parts[0], 9);
    nc_update_manifest_free(&manifest);

    {
        const char *invalid[] = {
            "{\"schemaVersion\":1,\"version\":\"9.9.9\",\"installer\":{\"url\":\"https://e/x\",\"sha256\":\"AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA\",\"size\":1}}",
            "{\"schemaVersion\":2,\"version\":\"bad\",\"installer\":{\"url\":\"https://e/x\",\"sha256\":\"AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA\",\"size\":1}}",
            "{\"schemaVersion\":2,\"version\":\"9.9.9\"}",
            "{\"schemaVersion\":2,\"version\":\"9.9.9\",\"installer\":{\"url\":\"http://e/x\",\"sha256\":\"AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA\",\"size\":1}}",
            "{\"schemaVersion\":2,\"version\":\"9.9.9\",\"installer\":{\"url\":\"https://e/x\",\"sha256\":\"zz\",\"size\":1}}",
            "{\"schemaVersion\":2,\"version\":\"9.9.9\",\"installer\":{\"url\":\"https://e/x\",\"sha256\":\"AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA\",\"size\":0}}",
            "{\"schemaVersion\":2,\"version\":\"9.9.9\",\"installer\":{\"url\":\"https://e/x\",\"sha256\":\"AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA\",\"size\":1},\"pageUrl\":\"not a url\"}",
            "not json"
        };
        size_t index;
        for (index = 0; index < sizeof(invalid) / sizeof(invalid[0]); index++) {
            CHECK(!nc_update_parse_manifest(invalid[index], strlen(invalid[index]), &manifest,
                                            &version, &error));
        }
    }

    {
        WStr path;
        wstr_init(&path);
        CHECK(nc_update_payload_path_w("9.9.9", &path));
        CHECK(wcsstr(wstr_cstr(&path), L"NetConditioner-9.9.9-win-x64.exe") != NULL);
        wstr_free(&path);
    }

    {
        NcSettings settings;
        memset(&settings, 0, sizeof(settings));
        settings.last_update_check_unix = 0;
        CHECK(nc_should_check(&settings, 100000));
        settings.last_update_check_unix = 100000 - 23 * 60 * 60;
        CHECK(!nc_should_check(&settings, 100000));
        settings.last_update_check_unix = 100000 - 24 * 60 * 60;
        CHECK(nc_should_check(&settings, 100000));
        settings.last_update_check_unix = 200000;
        CHECK(nc_should_check(&settings, 100000));
    }

    {
        wchar_t *argv[4];
        argv[0] = L"NetConditioner.exe";
        argv[1] = L"--apply-update";
        argv[2] = L"--target";
        argv[3] = L"x";
        CHECK(nc_selfupdate_is_helper_mode(4, argv));
        argv[1] = L"--self-test";
        CHECK(!nc_selfupdate_is_helper_mode(4, argv));
        CHECK_INT(nc_selfupdate_run_helper(1, argv), 2);
    }

    SetEnvironmentVariableA("NC_UPDATE_DRY_RUN", "1");
    CHECK(nc_update_is_dry_run());
    SetEnvironmentVariableA("NC_UPDATE_DRY_RUN", NULL);
    CHECK(!nc_update_is_dry_run());
}
