// swm - runtime configuration loader.
//
// Reads an XDG-resolved config file ($XDG_CONFIG_HOME/swm/swm.conf, falling
// back to $HOME/.config/swm/swm.conf). If the file does not exist, a default
// is written so the user has an editable template. Everything (modifier,
// border colors/width, gap, and the full keybinding table) is user-editable
// without recompiling.
//
// Config syntax:
//
//   [settings]
//   mod = SUPER                     # primary modifier for the MOD alias
//   border_width = 1
//   gap = 0
//   border_normal = #333333
//   border_select = #c831dc
//
//   [bindings]
//   <MODS>+<KEY> = <action> [args]
//
// Actions: kill, center, fullscreen, next, prev,
//          workspace <n>, send <n>, exec <cmd...>

#include "swm.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <limits.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <xkbcommon/xkbcommon.h>

/* Runtime config state (defined here, declared in swm.h). */
int border_width = 1;
int gap_size = 0;
uint32_t config_mod = WLR_MODIFIER_LOGO;
key *keys = NULL;
size_t nkeys = 0;

/* Default configuration, written on first run. */
static const char *DEFAULT_CONFIG =
"# swm configuration\n"
"# Modifiers: SUPER (Logo/Windows), ALT, SHIFT, CTRL, or MOD (primary mod)\n"
"# Actions: kill, center, fullscreen, next, prev, workspace <n>, send <n>, exec <cmd...>\n"
"\n"
"[settings]\n"
"\n"
"mod = SUPER\n"
"border_width = 1\n"
"gap = 0\n"
"border_normal = #333333\n"
"border_select = #c831dc\n"
"\n"
"[bindings]\n"
"\n"
"# Window ops\n"
"SUPER+q = kill\n"
"SUPER+c = center\n"
"SUPER+f = fullscreen\n"
"ALT+Tab = next\n"
"ALT+SHIFT+Tab = prev\n"
"\n"
"# Launchers\n"
"SUPER+d = exec srun\n"
"SUPER+w = exec bud /home/vwinter/Wallpapers\n"
"SUPER+p = exec scr\n"
"SUPER+Return = exec st\n"
"\n"
"# Media keys\n"
"XF86AudioLowerVolume = exec amixer sset Master 5%-\n"
"XF86AudioRaiseVolume = exec amixer sset Master 5%+\n"
"XF86AudioMute = exec amixer sset Master toggle\n"
"XF86MonBrightnessUp = exec bri 10 +\n"
"XF86MonBrightnessDown = exec bri 10 -\n"
"\n"
"# Workspaces\n"
"SUPER+1 = workspace 1\n"
"SUPER+SHIFT+1 = send 1\n"
"SUPER+2 = workspace 2\n"
"SUPER+SHIFT+2 = send 2\n"
"SUPER+3 = workspace 3\n"
"SUPER+SHIFT+3 = send 3\n"
"SUPER+4 = workspace 4\n"
"SUPER+SHIFT+4 = send 4\n"
"SUPER+5 = workspace 5\n"
"SUPER+SHIFT+5 = send 5\n"
"SUPER+6 = workspace 6\n"
"SUPER+SHIFT+6 = send 6\n";

/* ----- small helpers ----- */

static char *trim(char *s) {
	while (*s && isspace((unsigned char)*s)) s++;
	if (!*s) return s;
	char *e = s + strlen(s) - 1;
	while (e > s && isspace((unsigned char)*e)) *e-- = 0;
	return s;
}

static void mkdir_p(const char *path) {
	char tmp[PATH_MAX];
	size_t len = strlen(path);
	if (len >= sizeof(tmp)) return;
	memcpy(tmp, path, len + 1);
	for (char *p = tmp + 1; *p; p++) {
		if (*p == '/') {
			*p = 0;
			mkdir(tmp, 0755);
			*p = '/';
		}
	}
	mkdir(tmp, 0755);
}

/* Resolve the config file path under XDG, creating the directory. Caller
 * frees the result. */
static char *config_file_path(void) {
	const char *xdg = getenv("XDG_CONFIG_HOME");
	const char *home = getenv("HOME");
	char *base;
	if (xdg && *xdg) {
		size_t n = strlen(xdg) + strlen("/swm/swm.conf") + 1;
		base = malloc(n);
		snprintf(base, n, "%s/swm/swm.conf", xdg);
	} else if (home && *home) {
		size_t n = strlen(home) + strlen("/.config/swm/swm.conf") + 1;
		base = malloc(n);
		snprintf(base, n, "%s/.config/swm/swm.conf", home);
	} else {
		base = strdup(".config/swm/swm.conf");
	}
	char *slash = strrchr(base, '/');
	if (slash) {
		*slash = 0;
		mkdir_p(base);
		*slash = '/';
	} else {
		mkdir_p(".");
	}
	return base;
}

/* ----- modifier + keysym parsing ----- */

static int mod_token(const char *t, uint32_t *m) {
	if (!strcasecmp(t, "SUPER") || !strcasecmp(t, "LOGO"))   { *m = WLR_MODIFIER_LOGO;  return 1; }
	if (!strcasecmp(t, "ALT")   || !strcasecmp(t, "MOD1"))   { *m = WLR_MODIFIER_ALT;   return 1; }
	if (!strcasecmp(t, "SHIFT"))                            { *m = WLR_MODIFIER_SHIFT; return 1; }
	if (!strcasecmp(t, "CTRL")  || !strcasecmp(t, "CONTROL")){ *m = WLR_MODIFIER_CTRL;  return 1; }
	if (!strcasecmp(t, "MOD2"))                             { *m = WLR_MODIFIER_MOD2;  return 1; }
	if (!strcasecmp(t, "MOD3"))                             { *m = WLR_MODIFIER_MOD3;  return 1; }
	if (!strcasecmp(t, "MOD5"))                             { *m = WLR_MODIFIER_MOD5;  return 1; }
	if (!strcasecmp(t, "MOD"))                              { *m = config_mod;        return 1; }
	return 0;
}

static uint32_t parse_mod_spec(char *v) {
	uint32_t mask = 0;
	char *save = NULL;
	for (char *tok = strtok_r(v, "+ \t", &save); tok;
	     tok = strtok_r(NULL, "+ \t", &save)) {
		uint32_t m;
		if (mod_token(tok, &m)) mask |= m;
	}
	return mask;
}

/* left = "MODS+KEY"; fill k->mod and k->keysym. Returns 1 on success. */
static int parse_binding(char *left, key *k) {
	uint32_t mods = 0;
	char *keyname = NULL, *last = NULL, *save = NULL;
	for (char *tok = strtok_r(left, "+", &save); tok;
	     tok = strtok_r(NULL, "+", &save)) {
		uint32_t m;
		if (mod_token(tok, &m)) mods |= m;
		else keyname = tok;
		last = tok;
	}
	if (!keyname) keyname = last;
	if (!keyname) return 0;

	xkb_keysym_t ks = xkb_keysym_from_name(keyname, XKB_KEYSYM_CASE_INSENSITIVE);
	if (ks == XKB_KEY_NoSymbol) return 0;
	k->mod = mods;
	k->keysym = ks;
	return 1;
}

/* Split the remainder of an exec command into a NULL-terminated argv,
 * honoring double quotes. Caller frees via the returned pointer. */
static char **cmd_argv(char *s) {
	char **argv = NULL;
	size_t n = 0, cap = 0;
	char *p = s;
	while (*p) {
		while (*p && isspace((unsigned char)*p)) p++;
		if (!*p) break;
		size_t len = 0;
		char *tok = malloc(strlen(p) + 1);
		if (!tok) goto fail;
		int inq = 0;
		while (*p && (inq || !isspace((unsigned char)*p))) {
			if (*p == '"') { inq = !inq; p++; continue; }
			tok[len++] = *p++;
		}
		tok[len] = 0;
		if (n == cap) {
			cap = cap ? cap * 2 : 4;
			char **new_argv = realloc(argv, cap * sizeof(char *));
			if (!new_argv) { free(tok); goto fail; }
			argv = new_argv;
		}
		argv[n++] = tok;
	}
	{
		char **new_argv = realloc(argv, (n + 1) * sizeof(char *));
		if (new_argv) argv = new_argv;
	}
	if (argv) argv[n] = NULL;
	return argv;

fail:
	for (size_t i = 0; i < n; i++) free(argv[i]);
	free(argv);
	return NULL;
}

/* right = "<action> [args]"; fill k->function and k->arg. Returns 1 on success. */
static int parse_action(char *right, key *k) {
	char *save = NULL;
	char *act = strtok_r(right, " \t", &save);
	if (!act) return 0;

	if (!strcasecmp(act, "kill"))       { k->function = win_kill;   return 1; }
	if (!strcasecmp(act, "center"))     { k->function = win_center; return 1; }
	if (!strcasecmp(act, "fullscreen")) { k->function = win_fs;     return 1; }
	if (!strcasecmp(act, "next"))       { k->function = win_next;   return 1; }
	if (!strcasecmp(act, "prev"))       { k->function = win_prev;   return 1; }

	if (!strcasecmp(act, "workspace") || !strcasecmp(act, "goto")) {
		char *v = strtok_r(NULL, " \t", &save);
		int w = v ? atoi(v) : 0;
		if (w < 1 || w > NUM_WS) return 0;
		k->function = ws_go; k->arg.i = w;
		return 1;
	}
	if (!strcasecmp(act, "send")) {
		char *v = strtok_r(NULL, " \t", &save);
		int w = v ? atoi(v) : 0;
		if (w < 1 || w > NUM_WS) return 0;
		k->function = win_to_ws; k->arg.i = w;
		return 1;
	}
	if (!strcasecmp(act, "exec")) {
		char **argv = cmd_argv(save);
		if (!argv || !argv[0]) { free(argv); return 0; }
		k->function = run;
		k->arg.com = (const char **)argv;
		return 1;
	}
	return 0;
}

static void add_key(key k) {
	if (nkeys % 16 == 0) {
		key *new_keys = realloc(keys, (nkeys + 16) * sizeof(key));
		if (!new_keys) {
			fprintf(stderr, "swm: failed to allocate keybindings\n");
			return;
		}
		keys = new_keys;
	}
	keys[nkeys++] = k;
}

/* ----- line parsing ----- */

static void parse_setting(char *left, char *right) {
	if (!strcmp(left, "mod")) {
		config_mod = parse_mod_spec(right);
	} else if (!strcmp(left, "border_width") || !strcmp(left, "borderwidth")) {
		border_width = atoi(right);
	} else if (!strcmp(left, "gap")) {
		gap_size = atoi(right);
	} else if (!strcmp(left, "border_normal") || !strcmp(left, "bordernormal")) {
		hex_to_rgba(right, normal_rgba);
	} else if (!strcmp(left, "border_select") || !strcmp(left, "borderselect")) {
		hex_to_rgba(right, focus_rgba);
	} else {
		fprintf(stderr, "swm: unknown setting: %s\n", left);
	}
}

static void parse_binding_line(char *left, char *right) {
	key k = {0};
	if (parse_binding(left, &k) && parse_action(right, &k))
		add_key(k);
	else
		fprintf(stderr, "swm: ignoring bad binding: %s = %s\n", left, right);
}

/* Section context for organizing the config file. */
enum { SECTION_NONE, SECTION_SETTINGS, SECTION_BINDINGS };

/* Parse one line, updating *section when a [section] header is seen. */
static void parse_line(char *line, int *section) {
	/* Strip comments */
	char *hash = strchr(line, '#');
	if (hash) *hash = 0;

	char *trimmed = trim(line);
	if (!*trimmed) return;

	/* Section headers */
	if (*trimmed == '[') {
		char *end = strchr(trimmed, ']');
		if (!end) return;
		*end = 0;
		char *name = trim(trimmed + 1);
		if (!strcasecmp(name, "settings"))
			*section = SECTION_SETTINGS;
		else if (!strcasecmp(name, "bindings"))
			*section = SECTION_BINDINGS;
		return;
	}

	char *eq = strchr(trimmed, '=');
	if (!eq) return;
	*eq = 0;
	char *left = trim(trimmed);
	char *right = trim(eq + 1);
	if (!*left || !*right) return;

	if (*section == SECTION_SETTINGS) {
		parse_setting(left, right);
	} else {
		parse_binding_line(left, right);
	}
}

static void parse_string(const char *s) {
	char *buf = strdup(s);
	char *save = NULL;
	int section = SECTION_BINDINGS;
	for (char *line = strtok_r(buf, "\n", &save); line;
	     line = strtok_r(NULL, "\n", &save))
		parse_line(line, &section);
	free(buf);
}

static void parse_stream(FILE *f) {
	char buf[4096];
	int section = SECTION_BINDINGS;
	while (fgets(buf, sizeof buf, f)) {
		size_t len = strlen(buf);
		/* If the line doesn't end with newline it was truncated; skip it. */
		if (len > 0 && buf[len - 1] != '\n') {
			fprintf(stderr, "swm: config line too long, skipping\n");
			int c;
			while ((c = fgetc(f)) != EOF && c != '\n');
			continue;
		}
		parse_line(buf, &section);
	}
}

static void write_default_config(const char *path) {
	FILE *f = fopen(path, "w");
	if (!f) return;
	fputs(DEFAULT_CONFIG, f);
	fclose(f);
}

/* ----- public entry point ----- */

void load_config(void) {
	/* Sensible defaults; overridden by the config file. */
	border_width = 1;
	gap_size = 0;
	config_mod = WLR_MODIFIER_LOGO;
	hex_to_rgba("#333333", normal_rgba);
	hex_to_rgba("#c831dc", focus_rgba);

	char *path = config_file_path();
	FILE *f = fopen(path, "r");
	if (!f) {
		write_default_config(path);
		f = fopen(path, "r");
	}
	if (f) {
		parse_stream(f);
		fclose(f);
	} else {
		parse_string(DEFAULT_CONFIG);
	}
	free(path);

	if (nkeys == 0) {
		fprintf(stderr,
			"swm: no valid keybindings in config; using built-in defaults\n");
		parse_string(DEFAULT_CONFIG);
	}
}

#ifdef TESTING
/* Expose the static parser for unit tests so we can feed arbitrary config
 * strings and verify the resulting keybinding table and settings. */
void test_parse_string(const char *s) { parse_string(s); }
void test_reset_state(void) {
	border_width = 1;
	gap_size = 0;
	config_mod = WLR_MODIFIER_LOGO;
	nkeys = 0;
}
int  test_nkeys(void) { return (int)nkeys; }
key *test_keys(void)  { return keys; }
int  test_border_width(void) { return border_width; }
int  test_gap(void) { return gap_size; }
uint32_t test_config_mod(void) { return config_mod; }
#endif
