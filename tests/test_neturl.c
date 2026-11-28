#include "test_harness.h"

#include "core/neturl.h"

#include <string.h>

static void check_feed(const char *url, bool expected) {
    NcErr error;
    bool ok = nc_url_validate_feed(url, &error);
    CHECK(ok == expected);
}

static void check_redirect(const char *current_text, const char *next_text, bool expected) {
    NcUrl current;
    NcUrl next;
    NcErr error;
    if (!nc_url_parse(current_text, "更新地址", &current, &error)) {
        CHECK(!expected);
        return;
    }
    if (!nc_url_parse(next_text, "重定向地址", &next, &error)) {
        CHECK(!expected);
        return;
    }
    CHECK(nc_url_validate_redirect(&current, &next, &error) == expected);
}

void test_neturl_run(void) {
    NcErr error;
    NcUrl url;
    NcUrl joined;
    char buffer[256];
    char sha[65];

    check_feed("https://example.com/latest.json", true);
    check_feed("http://127.0.0.1:8080/latest.json", true);
    check_feed("http://localhost:8080/latest.json", true);
    check_feed("http://example.com/latest.json", false);
    check_feed("ftp://example.com/latest.json", false);
    check_feed("not a url", false);
    check_feed("", false);
    CHECK(!nc_url_validate_feed("", &error));
    CHECK_STR(error.msg, "未配置更新地址。");
    {
        char long_url[2200];
        size_t index;
        const char *prefix = "https://example.com/";
        strcpy(long_url, prefix);
        for (index = strlen(prefix); index < sizeof(long_url) - 1; index++) {
            long_url[index] = 'a';
        }
        long_url[sizeof(long_url) - 1] = 0;
        CHECK(!nc_url_validate_feed(long_url, &error));
        CHECK_STR(error.msg, "更新地址过长。");
    }
    CHECK(!nc_url_validate_feed("https://user:secret@example.com/feed", &error));
    CHECK(strstr(error.msg, "用户信息") != NULL);
    CHECK(strstr(error.msg, "secret") == NULL);

    check_redirect("https://example.com/feed", "https://cdn.example.com/a", true);
    check_redirect("https://example.com/feed", "http://example.com/a", false);
    check_redirect("https://example.com/feed", "https://127.0.0.1/a", false);
    check_redirect("http://127.0.0.1:8080/feed", "http://localhost:8081/a", true);
    check_redirect("http://127.0.0.1:8080/feed", "https://localhost:8081/a", true);
    check_redirect("http://127.0.0.1:8080/feed", "http://192.168.1.2/a", false);

    CHECK(nc_ip_text_is_global("8.8.8.8"));
    CHECK(!nc_ip_text_is_global("10.0.0.1"));
    CHECK(!nc_ip_text_is_global("127.0.0.1"));
    CHECK(!nc_ip_text_is_global("169.254.1.1"));
    CHECK(!nc_ip_text_is_global("192.168.1.1"));
    CHECK(!nc_ip_text_is_global("224.0.0.1"));
    CHECK(!nc_ip_text_is_global("0.0.0.0"));
    CHECK(!nc_ip_text_is_global("255.255.255.255"));
    CHECK(!nc_ip_text_is_global("192.0.2.1"));
    CHECK(!nc_ip_text_is_global("198.51.100.1"));
    CHECK(!nc_ip_text_is_global("203.0.113.1"));
    CHECK(nc_ip_text_is_global("100.64.0.1"));
    CHECK(nc_ip_text_is_global("2001:4860::8888"));
    CHECK(!nc_ip_text_is_global("::1"));
    CHECK(!nc_ip_text_is_global("fc00::1"));
    CHECK(!nc_ip_text_is_global("fe80::1"));
    CHECK(!nc_ip_text_is_global("::"));
    CHECK(!nc_ip_text_is_global("ff02::1"));

    CHECK(nc_url_parse("https://example.com/a/b?x=1#frag", "更新地址", &url, &error));
    CHECK_STR(url.scheme, "https");
    CHECK_STR(url.host, "example.com");
    CHECK_STR(url.path, "/a/b?x=1");
    CHECK_INT(url.port, 0);
    CHECK(nc_url_parse("http://[::1]:8080/x", "更新地址", &url, &error));
    CHECK_STR(url.host, "::1");
    CHECK_INT(url.port, 8080);
    CHECK(nc_url_is_loopback(&url));

    CHECK(nc_url_parse("https://example.com/a/b?x=1", "更新地址", &url, &error));
    CHECK(nc_url_join(&url, "c", &joined, &error));
    CHECK_STR(joined.path, "/a/c");
    CHECK(nc_url_join(&url, "/d", &joined, &error));
    CHECK_STR(joined.path, "/d");
    CHECK(nc_url_join(&url, "?q=2", &joined, &error));
    CHECK_STR(joined.path, "/a/b?q=2");
    CHECK(nc_url_join(&url, "https://other.example/z", &joined, &error));
    CHECK_STR(joined.host, "other.example");

    CHECK(nc_url_redact("https://example.com/a?b=1#frag", buffer, sizeof(buffer)));
    CHECK_STR(buffer, "https://example.com/a?b=1");
    CHECK(!nc_url_redact("not a url", buffer, sizeof(buffer)));
    CHECK_STR(buffer, "(无效地址)");

    {
        char lower[65];
        size_t index;
        for (index = 0; index < 64; index++) {
            lower[index] = 'a';
        }
        lower[64] = 0;
        CHECK(nc_neturl_normalize_sha256(lower, sha));
        CHECK_STR(sha, "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA");
        CHECK(nc_neturl_normalize_sha256("sha256: aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", sha));
    }
    CHECK(!nc_neturl_normalize_sha256("abc", sha));
    CHECK(!nc_neturl_normalize_sha256("zzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzz", sha));
}
