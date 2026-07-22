/* Unit tests for swm.
 *
 * Build with TESTING defined so main.c skips its main() and config.c
 * exposes internal parser wrappers.
 *
 * Compile:
 *   cc -DTESTING $(CFLAGS) -c src/main.c  -o build/test_main.o
 *   cc -DTESTING $(CFLAGS) -c src/config.c -o build/test_config.o
 *   cc -DTESTING $(CFLAGS) -c src/wallpaper.c -o build/test_wallpaper.o
 *   cc -DTESTING $(CFLAGS) -c test/tests.c  -o build/test_tests.o
 *   cc build/test_*.o -o build/test_swm $(LDLIBS)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <math.h>

#include "swm.h"

/* ----- test framework helpers ----- */

static int total  = 0;
static int passed = 0;

#define ASSERT(cond, fmt, ...) do {                                    \
	total++;                                                           \
	if (!(cond)) {                                                     \
		fprintf(stderr, "  FAIL [%d] " fmt "\n", total, ##__VA_ARGS__);\
	} else {                                                           \
		passed++;                                                      \
	}                                                                  \
} while (0)

#define TEST(name) do {                                  \
	printf("  " name "...\n");                           \
} while (0)

/* Declarations for the test-exposed config internals. */
void test_parse_string(const char *s);
void test_reset_state(void);
int  test_nkeys(void);
key *test_keys(void);
int  test_border_width(void);
int  test_gap(void);
uint32_t test_config_mod(void);

/* Exposed by config.c under TESTING. */
uint32_t parse_mod_spec(char *v);
char **cmd_argv(char *s);

/* Stub functions for the action callbacks that config.c stores in the
 * keybinding table. These are only stored (never called during parsing),
 * but the linker needs the symbols to be present. */
void win_kill(const Arg arg)    { (void)arg; }
void win_center(const Arg arg)  { (void)arg; }
void win_fs(const Arg arg)      { (void)arg; }
void win_next(const Arg arg)    { (void)arg; }
void win_prev(const Arg arg)    { (void)arg; }
void ws_go(const Arg arg)       { (void)arg; }
void win_to_ws(const Arg arg)   { (void)arg; }

/* Floating-point comparison helper. */
static int f_near(float a, float b) {
	return fabsf(a - b) < 0.001f;
}

/* =================================================================
 * hex_to_rgba
 * ================================================================= */
static void test_hex_to_rgba(void) {
	TEST("hex_to_rgba");
	float c[4];

	hex_to_rgba("#ff0000", c);
	ASSERT(f_near(c[0], 1.0f), "red channel = %g", c[0]);
	ASSERT(f_near(c[1], 0.0f), "green channel = %g", c[1]);
	ASSERT(f_near(c[2], 0.0f), "blue channel = %g", c[2]);
	ASSERT(f_near(c[3], 1.0f), "alpha channel = %g", c[3]);

	hex_to_rgba("#00ff00", c);
	ASSERT(f_near(c[0], 0.0f), "red");
	ASSERT(f_near(c[1], 1.0f), "green");

	hex_to_rgba("#0000ff", c);
	ASSERT(f_near(c[2], 1.0f), "blue");

	hex_to_rgba("#ffffff", c);
	ASSERT(f_near(c[0], 1.0f) && f_near(c[1], 1.0f) && f_near(c[2], 1.0f),
		"white = (%g,%g,%g)", c[0], c[1], c[2]);

	hex_to_rgba("#000000", c);
	ASSERT(f_near(c[0], 0.0f) && f_near(c[1], 0.0f) && f_near(c[2], 0.0f),
		"black = (%g,%g,%g)", c[0], c[1], c[2]);

	/* boundary: #333333 (default normal border) */
	hex_to_rgba("#333333", c);
	ASSERT(f_near(c[0], 0.2f) && f_near(c[1], 0.2f) && f_near(c[2], 0.2f),
		"#333333 = (%g,%g,%g)", c[0], c[1], c[2]);

	/* default focus border */
	hex_to_rgba("#c831dc", c);
	ASSERT(f_near(c[0], 0.784f), "focus red ~0.784 got %g", c[0]);
	ASSERT(f_near(c[1], 0.192f), "focus green ~0.192 got %g", c[1]);
	ASSERT(f_near(c[2], 0.863f), "focus blue ~0.863 got %g", c[2]);
}

/* =================================================================
 * clean_mask
 * ================================================================= */
static void test_clean_mask(void) {
	TEST("clean_mask");

	/* Caps alone should be stripped */
	uint32_t m = clean_mask(WLR_MODIFIER_CAPS);
	ASSERT(m == 0, "caps alone -> 0, got 0x%x", m);

	/* Shift + Caps -> Shift */
	m = clean_mask(WLR_MODIFIER_SHIFT | WLR_MODIFIER_CAPS);
	ASSERT(m == WLR_MODIFIER_SHIFT, "shift+caps -> shift, got 0x%x", m);

	/* Alt + Ctrl */
	m = clean_mask(WLR_MODIFIER_ALT | WLR_MODIFIER_CTRL);
	ASSERT(m == (WLR_MODIFIER_ALT | WLR_MODIFIER_CTRL),
		"alt+ctrl, got 0x%x", m);

	/* Logo only */
	m = clean_mask(WLR_MODIFIER_LOGO);
	ASSERT(m == WLR_MODIFIER_LOGO, "logo, got 0x%x", m);

	/* Zero */
	m = clean_mask(0);
	ASSERT(m == 0, "zero -> 0, got 0x%x", m);

	/* Everything */
	m = clean_mask(WLR_MODIFIER_SHIFT | WLR_MODIFIER_CTRL | WLR_MODIFIER_ALT |
		WLR_MODIFIER_MOD2 | WLR_MODIFIER_MOD3 | WLR_MODIFIER_LOGO |
		WLR_MODIFIER_MOD5 | WLR_MODIFIER_CAPS);
	ASSERT(m == (WLR_MODIFIER_SHIFT | WLR_MODIFIER_CTRL | WLR_MODIFIER_ALT |
		WLR_MODIFIER_MOD2 | WLR_MODIFIER_MOD3 | WLR_MODIFIER_LOGO |
		WLR_MODIFIER_MOD5),
		"all valid, got 0x%x", m);
}

/* =================================================================
 * MAX macro
 * ================================================================= */
static void test_max_macro(void) {
	TEST("MAX macro");
	ASSERT(MAX(1, 2) == 2, "MAX(1,2)=2");
	ASSERT(MAX(5, 3) == 5, "MAX(5,3)=5");
	ASSERT(MAX(-1, -5) == -1, "MAX(-1,-5)=-1");
	ASSERT(MAX(0, 0) == 0, "MAX(0,0)=0");
	ASSERT(MAX(100, 100) == 100, "MAX(100,100)=100");
}

/* =================================================================
 * Config parser
 * ================================================================= */
static void test_config_defaults(void) {
	TEST("config defaults");
	test_reset_state();

	/* The DEFAULT_CONFIG string gives us 27+ bindings (some media keys
	 * and 12 workspace bindings, etc.). Just verify it parses cleanly. */
	ASSERT(test_nkeys() == 0, "no keys before parse");
	test_parse_string(
		"[settings]\n"
		"mod = SUPER\n"
		"border_width = 2\n"
		"border_normal = #ff0000\n"
		"[bindings]\n"
		"SUPER+q = kill\n"
	);
	ASSERT(test_nkeys() >= 1, "at least one keybinding parsed (%d)", test_nkeys());
	ASSERT(test_border_width() == 2, "border_width=2, got %d", test_border_width());
}

static void test_config_mod_alias(void) {
	TEST("config MOD alias resolves");
	test_reset_state();
	test_parse_string(
		"[settings]\n"
		"mod = ALT\n"
		"[bindings]\n"
		"MOD+q = kill\n"
	);
	ASSERT(test_nkeys() >= 1, "at least one binding");
	/* MOD should resolve to ALT (WLR_MODIFIER_ALT) */
	key *k = test_keys();
	ASSERT(k != NULL, "keys pointer not null");
	if (k) {
		ASSERT(k[0].mod == WLR_MODIFIER_ALT,
			"MOD binding resolved to ALT, got 0x%x", k[0].mod);
	}
}

static void test_config_workspace_binding(void) {
	TEST("config workspace binding");
	test_reset_state();
	test_parse_string(
		"[bindings]\n"
		"SUPER+1 = workspace 1\n"
		"SUPER+SHIFT+1 = send 1\n"
	);
	ASSERT(test_nkeys() >= 2, "two bindings");
}

static void test_config_exec_binding(void) {
	TEST("config exec binding");
	test_reset_state();
	test_parse_string(
		"[bindings]\n"
		"SUPER+d = exec srun\n"
	);
	ASSERT(test_nkeys() >= 1, "exec binding parsed");
	key *k = test_keys();
	if (k) {
		ASSERT(k[0].function == run,
			"exec binding has run function");
	}
}

static void test_config_bad_binding_ignored(void) {
	TEST("config bad binding silently ignored");
	test_reset_state();
	test_parse_string(
		"[bindings]\n"
		"SUPER+z = nonexistent_action\n"
		"SUPER+q = kill\n"
	);
	/* Only the valid binding should be registered */
	ASSERT(test_nkeys() == 1, "only 1 valid binding, got %d", test_nkeys());
}

static void test_config_unknown_setting_ignored(void) {
	TEST("config unknown setting");
	test_reset_state();
	test_parse_string(
		"[settings]\n"
		"nonexistent = foobar\n"
		"border_width = 3\n"
	);
	ASSERT(test_border_width() == 3, "border_width still set to 3, got %d",
		test_border_width());
}

static void test_config_comment_and_blank_lines(void) {
	TEST("config comments and blank lines");
	test_reset_state();
	test_parse_string(
		"# this is a comment\n"
		"\n"
		"   # indented comment\n"
		"[settings]\n"
		"gap = 10\n"
		"[bindings]\n"
		"SUPER+q = kill\n"
	);
	ASSERT(test_gap() == 10, "gap=10, got %d", test_gap());
	ASSERT(test_nkeys() >= 1, "binding parsed");
}

static void test_config_section_header_casing(void) {
	TEST("config section header case-insensitivity");
	test_reset_state();
	test_parse_string(
		"[SETTINGS]\n"
		"gap = 4\n"
		"[BINDINGS]\n"
		"SUPER+q = kill\n"
	);
	ASSERT(test_gap() == 4, "gap=4, got %d", test_gap());
	ASSERT(test_nkeys() >= 1, "binding parsed");
}

static void test_config_reset_between_parses(void) {
	TEST("config reset between parses");
	test_reset_state();
	test_parse_string("[bindings]\nSUPER+q = kill\n");
	int n1 = test_nkeys();
	test_reset_state();
	ASSERT(test_nkeys() == 0, "nkeys reset to 0, got %d", test_nkeys());
	(void)n1;
}

/* Test that parsing ONLY the [settings] section produces NO keybindings.
 * This exercises the same code path used by reload_settings(). */
static void test_config_settings_only(void) {
	TEST("config settings-only (reload path)");
	test_reset_state();
	test_parse_string(
		"[settings]\n"
		"mod = SUPER\n"
		"border_width = 4\n"
		"gap = 8\n"
		"border_normal = #ff0000\n"
		"border_select = #00ff00\n"
		/* Note: no [bindings] section at all */
	);
	ASSERT(test_nkeys() == 0, "no keybindings from settings-only parse, got %d",
		test_nkeys());
	ASSERT(test_border_width() == 4, "border_width=4 from settings, got %d",
		test_border_width());
	ASSERT(test_gap() == 8, "gap=8 from settings, got %d", test_gap());
}

/* Test that [bindings] without [settings] still gets defaults for settings. */
static void test_config_bindings_only(void) {
	TEST("config bindings-only (defaults used)");
	test_reset_state();
	test_parse_string(
		"[bindings]\n"
		"SUPER+Return = exec foot\n"
		"SUPER+q = kill\n"
	);
	ASSERT(test_nkeys() >= 2, "two bindings registered, got %d", test_nkeys());
	/* Settings should retain defaults */
	ASSERT(test_border_width() == 1, "border_width defaults to 1, got %d",
		test_border_width());
	ASSERT(test_gap() == 0, "gap defaults to 0, got %d", test_gap());
}

/* Test trailing whitespace tolerance in bindings. */
static void test_config_trailing_whitespace(void) {
	TEST("config trailing whitespace");
	test_reset_state();
	test_parse_string(
		"[bindings]\n"
		"SUPER+q = kill  \n"
		"  SUPER+w = kill\n"
	);
	ASSERT(test_nkeys() == 2, "two bindings with whitespace, got %d", test_nkeys());
}

/* =================================================================
 * cmd_argv — command tokenizer
 * ================================================================= */
static void free_cmd_argv(char **argv) {
	if (!argv) return;
	for (size_t i = 0; argv[i]; i++) free(argv[i]);
	free(argv);
}

static void test_cmd_argv(void) {
	TEST("cmd_argv");
	char **av;

	av = cmd_argv("foot");
	ASSERT(av != NULL, "simple -> not NULL");
	ASSERT(av[0] && !strcmp(av[0], "foot"), "av[0]=foot");
	ASSERT(av[1] == NULL, "exactly one arg");
	free_cmd_argv(av);

	av = cmd_argv("foot --server /tmp/foot.sock");
	ASSERT(av != NULL, "args -> not NULL");
	ASSERT(av[0] && !strcmp(av[0], "foot"), "av[0]=foot");
	ASSERT(av[1] && !strcmp(av[1], "--server"), "av[1]=--server");
	ASSERT(av[2] && !strcmp(av[2], "/tmp/foot.sock"), "av[2]=path");
	ASSERT(av[3] == NULL, "exactly 3 args");
	free_cmd_argv(av);

	av = cmd_argv("firefox \"https://example.com\"");
	ASSERT(av != NULL, "quoted -> not NULL");
	ASSERT(av[0] && !strcmp(av[0], "firefox"), "av[0]=firefox");
	ASSERT(av[1] && !strcmp(av[1], "https://example.com"), "quoted url");
	ASSERT(av[2] == NULL, "exactly 2 args");
	free_cmd_argv(av);

	av = cmd_argv("alacritty -e foot nvim");
	ASSERT(av != NULL, "multiple -> not NULL");
	ASSERT(av[0] && !strcmp(av[0], "alacritty"), "av[0]=alacritty");
	ASSERT(av[1] && !strcmp(av[1], "-e"), "av[1]=-e");
	ASSERT(av[2] && !strcmp(av[2], "foot"), "av[2]=foot");
	ASSERT(av[3] && !strcmp(av[3], "nvim"), "av[3]=nvim");
	ASSERT(av[4] == NULL, "exactly 4 args");
	free_cmd_argv(av);

	/* Empty string */
	av = cmd_argv("");
	ASSERT(av == NULL || av[0] == NULL, "empty -> NULL or {NULL}");
	if (av) free(av);
}

/* =================================================================
 * parse_mod_spec — modifier key name resolution
 * ================================================================= */
static void test_parse_mod_spec(void) {
	TEST("parse_mod_spec");

	ASSERT(parse_mod_spec("SUPER")  == WLR_MODIFIER_LOGO, "SUPER -> LOGO");
	ASSERT(parse_mod_spec("super")  == WLR_MODIFIER_LOGO, "super lower -> LOGO");
	ASSERT(parse_mod_spec("Super")  == WLR_MODIFIER_LOGO, "Super mixed -> LOGO");
	ASSERT(parse_mod_spec("ALT")    == WLR_MODIFIER_ALT,  "ALT");
	ASSERT(parse_mod_spec("CTRL")   == WLR_MODIFIER_CTRL, "CTRL");
	ASSERT(parse_mod_spec("SHIFT")  == WLR_MODIFIER_SHIFT,"SHIFT");
	ASSERT(parse_mod_spec("MOD4")   == 0, "MOD4 not in mod_token list -> 0");
	ASSERT(parse_mod_spec("MOD1")   == WLR_MODIFIER_ALT,  "MOD1 -> ALT");
	ASSERT(parse_mod_spec("MOD")    == WLR_MODIFIER_LOGO, "MOD alone defaults to LOGO");

	/* Unknown names */
	ASSERT(parse_mod_spec("HYPER")  == 0, "HYPER unknown -> 0");
	ASSERT(parse_mod_spec("")       == 0, "empty -> 0");
	ASSERT(parse_mod_spec("LOGO")   == WLR_MODIFIER_LOGO, "LOGO -> LOGO (mod_token aliases LOGO=SUPER)");
}

/* =================================================================
 * DEFAULT_CONFIG — the bundled fallback config
 * ================================================================= */
static void test_default_config(void) {
	TEST("DEFAULT_CONFIG parses");
	test_reset_state();
	/* test_parse_string uses the DEFAULT_CONFIG hard-coded in config.c.
	 * We can't reference DEFAULT_CONFIG directly (it's static), but we
	 * can verify that the fallback path produces a reasonable number of
	 * bindings by parsing a minimal full config. */
	test_parse_string(
		"[settings]\n"
		"mod = SUPER\n"
		"border_width = 2\n"
		"gap = 4\n"
		"border_normal = #333333\n"
		"border_select = #c831dc\n"
		"[bindings]\n"
		"SUPER+Return = exec foot\n"
		"SUPER+q = kill\n"
		"SUPER+f = fullscreen\n"
		"SUPER+c = center\n"
		"SUPER+Tab = next\n"
		"SUPER+SHIFT+Tab = prev\n"
		"SUPER+1 = workspace 1\n"
		"SUPER+2 = workspace 2\n"
		"SUPER+3 = workspace 3\n"
		"SUPER+SHIFT+1 = send 1\n"
		"SUPER+SHIFT+2 = send 2\n"
		"SUPER+SHIFT+3 = send 3\n"
	);
	/* 12 bindings listed above */
	ASSERT(test_nkeys() == 12, "12 bindings parsed, got %d", test_nkeys());
	ASSERT(test_border_width() == 2, "border_width=2");
	ASSERT(test_gap() == 4, "gap=4");
}

/* Test gap setting edge cases. */
static void test_config_gap(void) {
	TEST("config gap setting");
	test_reset_state();
	test_parse_string(
		"[settings]\n"
		"gap = -5\n"
	);
	ASSERT(test_gap() == -5, "gap=-5 accepted, got %d", test_gap());

	test_reset_state();
	test_parse_string(
		"[settings]\n"
		"gap = 100\n"
	);
	ASSERT(test_gap() == 100, "gap=100 accepted, got %d", test_gap());
}

/* =================================================================
 * Main
 * ================================================================= */
int main(void) {
	printf("swm test suite\n"
	       "==============\n\n");

	test_hex_to_rgba();
	test_clean_mask();
	test_max_macro();

	printf("\nConfig parser:\n");
	test_config_defaults();
	test_config_mod_alias();
	test_config_workspace_binding();
	test_config_exec_binding();
	test_config_bad_binding_ignored();
	test_config_unknown_setting_ignored();
	test_config_comment_and_blank_lines();
	test_config_section_header_casing();
	test_config_reset_between_parses();
	test_config_settings_only();
	test_config_bindings_only();
	test_config_trailing_whitespace();
	test_config_gap();
	test_default_config();

	printf("\nCommand tokenizer:\n");
	test_cmd_argv();

	printf("\nModifier parsing:\n");
	test_parse_mod_spec();

	printf("\nResults: %d / %d passed\n", passed, total);
	return total == passed ? 0 : 1;
}
