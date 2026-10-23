/* Geometry checks independent of InkView, rendering, and daemon processes. */
#include "ui_layout.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(condition)                                                                           \
    do {                                                                                           \
        if (!(condition)) {                                                                        \
            fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition);                        \
            exit(1);                                                                               \
        }                                                                                          \
    } while (0)

static bool rectangles_overlap(ui_rect a, ui_rect b) {
    return a.x < b.x + b.width && b.x < a.x + a.width && a.y < b.y + b.height &&
           b.y < a.y + a.height;
}

static void check_named_controls(void) {
    for (ui_control_id id = UI_CONTROL_NONE + 1; id < UI_CONTROL_COUNT; ++id) {
        const char *name = ui_control_name(id);
        CHECK(name && *name);
        CHECK(ui_control_named(name) == id);
        ui_rect bounds = ui_control_bounds(id);
        CHECK(bounds.x >= 0 && bounds.y >= 0);
        CHECK(bounds.width > 0 && bounds.height > 0);
        CHECK(bounds.x + bounds.width <= UI_CANVAS_WIDTH);
        CHECK(bounds.y + bounds.height <= UI_CANVAS_HEIGHT);
        for (ui_control_id other = id + 1; other < UI_CONTROL_COUNT; ++other) {
            CHECK(strcmp(name, ui_control_name(other)) != 0);
        }
    }
    CHECK(ui_control_named("nonexistent") == UI_CONTROL_NONE);
    CHECK(ui_control_name(UI_CONTROL_NONE) == NULL);
    CHECK(ui_control_name(UI_CONTROL_COUNT) == NULL);
    CHECK(ui_control_bounds(UI_CONTROL_COUNT).width == 0);
}

static void check_text_regions(void) {
    for (ui_text_id id = 0; id < UI_TEXT_COUNT; ++id) {
        ui_rect bounds = ui_text_bounds(id);
        CHECK(bounds.x >= 0 && bounds.y >= 0);
        CHECK(bounds.width > 0 && bounds.height > 0);
        CHECK(bounds.x + bounds.width <= UI_CANVAS_WIDTH);
        CHECK(bounds.y + bounds.height <= UI_CANVAS_HEIGHT);
    }
    CHECK(ui_text_bounds(UI_TEXT_COUNT).width == 0);
}

static void check_hit_regions(ui_tab tab, const ec_config *config) {
    for (ui_control_id id = UI_CONTROL_NONE + 1; id < UI_CONTROL_COUNT; ++id) {
        if (!ui_control_visible(id, tab, config)) {
            continue;
        }
        ui_rect bounds = ui_control_bounds(id);
        CHECK(ui_hit_test(tab, config, bounds.x, bounds.y) == id);
        CHECK(ui_hit_test(tab, config, bounds.x + bounds.width / 2, bounds.y + bounds.height / 2) ==
              id);
        CHECK(ui_hit_test(tab, config, bounds.x + bounds.width - 1, bounds.y + bounds.height - 1) ==
              id);
        /* A control's right and bottom edges belong to neither its drawn area nor its hit area. */
        CHECK(ui_hit_test(tab, config, bounds.x + bounds.width, bounds.y) != id);
        CHECK(ui_hit_test(tab, config, bounds.x, bounds.y + bounds.height) != id);
        for (ui_control_id other = id + 1; other < UI_CONTROL_COUNT; ++other) {
            if (ui_control_visible(other, tab, config)) {
                CHECK(!rectangles_overlap(bounds, ui_control_bounds(other)));
            }
        }
    }
    CHECK(ui_hit_test(tab, config, -1, -1) == UI_CONTROL_NONE);
    CHECK(ui_hit_test(tab, config, UI_CANVAS_WIDTH, UI_CANVAS_HEIGHT) == UI_CONTROL_NONE);
}

static void check_visibility_and_gaps(void) {
    ec_config config = {0};
    CHECK(!ui_control_visible(UI_SSH_SET_PASSWORD, UI_TAB_SSH, &config));
    config.auth = EC_PASSWORD;
    CHECK(ui_control_visible(UI_SSH_SET_PASSWORD, UI_TAB_SSH, &config));
    CHECK(!ui_control_visible(UI_SSH_SET_PASSWORD, UI_TAB_FTP, &config));
    CHECK(!ui_control_visible(UI_FTP_PORT, UI_TAB_SSH, &config));
    CHECK(!ui_control_visible(UI_SSH_PORT, UI_TAB_FTP, &config));
    config.ftp_auth = EC_FTP_ANONYMOUS;
    CHECK(!ui_control_visible(UI_FTP_SET_PASSWORD, UI_TAB_FTP, &config));

    ui_rect left = ui_control_bounds(UI_SSH_KEYS);
    ui_rect right = ui_control_bounds(UI_SSH_PASSWORD);
    CHECK(left.x + left.width < right.x);
    int gap_x = (left.x + left.width + right.x) / 2;
    CHECK(ui_hit_test(UI_TAB_SSH, &config, gap_x, left.y + left.height / 2) == UI_CONTROL_NONE);
}

int main(void) {
    check_named_controls();
    check_text_regions();
    check_visibility_and_gaps();
    ec_config config = {0};
    for (ec_auth ssh_auth = EC_KEYS; ssh_auth <= EC_BOTH; ++ssh_auth) {
        config.auth = ssh_auth;
        for (ec_ftp_auth ftp_auth = EC_FTP_PASSWORD; ftp_auth <= EC_FTP_ANONYMOUS; ++ftp_auth) {
            config.ftp_auth = ftp_auth;
            check_hit_regions(UI_TAB_SSH, &config);
            check_hit_regions(UI_TAB_FTP, &config);
        }
    }
    puts("Layout bounds, names, visibility, gaps, and hit regions passed for every access mode.");
    return 0;
}
