/*
 * Rufus for Linux: locale, keyboard, code page and time zone information
 * Copyright © 2026 Rufus Linux port contributors
 *
 * On Windows, Rufus queries the system for the UI language (LCID), keyboard
 * layout (KLID), OEM code page and time zone. These are used to select the
 * UI translation, set up the DOS keyboard/code page for FreeDOS/MS-DOS media,
 * and duplicate the regional settings into Windows' unattend.xml. This file
 * derives the same values from the Linux environment.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */
#include <windows.h>
#include <msapi_utf8_linux.h>
#include <pwd.h>

#include "rufus.h"
#include "compat.h"

/* POSIX "ll_CC" -> Windows LCID */
static const struct { const char* name; uint16_t lcid; } locale_table[] = {
	{ "ar_SA", 0x0401 }, { "ar_EG", 0x0c01 }, { "ar_AE", 0x3801 }, { "ar_MA", 0x1801 }, { "ar", 0x0401 },
	{ "bg_BG", 0x0402 }, { "bg", 0x0402 },
	{ "ca_ES", 0x0403 }, { "ca", 0x0403 },
	{ "zh_TW", 0x0404 }, { "zh_HK", 0x0c04 }, { "zh_MO", 0x1404 }, { "zh_CN", 0x0804 }, { "zh_SG", 0x1004 }, { "zh", 0x0804 },
	{ "cs_CZ", 0x0405 }, { "cs", 0x0405 },
	{ "da_DK", 0x0406 }, { "da", 0x0406 },
	{ "de_DE", 0x0407 }, { "de_CH", 0x0807 }, { "de_AT", 0x0c07 }, { "de_LU", 0x1007 }, { "de_LI", 0x1407 }, { "de", 0x0407 },
	{ "el_GR", 0x0408 }, { "el", 0x0408 },
	{ "en_US", 0x0409 }, { "en_GB", 0x0809 }, { "en_AU", 0x0c09 }, { "en_CA", 0x1009 }, { "en_NZ", 0x1409 },
	{ "en_IE", 0x1809 }, { "en_ZA", 0x1c09 }, { "en_IN", 0x4009 }, { "en_SG", 0x4809 }, { "en", 0x0409 },
	{ "es_ES", 0x0c0a }, { "es_MX", 0x080a }, { "es_AR", 0x2c0a }, { "es_CO", 0x240a }, { "es_CL", 0x340a },
	{ "es_US", 0x540a }, { "es", 0x0c0a },
	{ "et_EE", 0x0425 }, { "et", 0x0425 },
	{ "fa_IR", 0x0429 }, { "fa", 0x0429 },
	{ "fi_FI", 0x040b }, { "fi", 0x040b },
	{ "fr_FR", 0x040c }, { "fr_BE", 0x080c }, { "fr_CA", 0x0c0c }, { "fr_CH", 0x100c }, { "fr_LU", 0x140c }, { "fr", 0x040c },
	{ "he_IL", 0x040d }, { "he", 0x040d }, { "iw_IL", 0x040d },
	{ "hi_IN", 0x0439 }, { "hi", 0x0439 },
	{ "hr_HR", 0x041a }, { "hr", 0x041a },
	{ "hu_HU", 0x040e }, { "hu", 0x040e },
	{ "id_ID", 0x0421 }, { "id", 0x0421 },
	{ "it_IT", 0x0410 }, { "it_CH", 0x0810 }, { "it", 0x0410 },
	{ "ja_JP", 0x0411 }, { "ja", 0x0411 },
	{ "ko_KR", 0x0412 }, { "ko", 0x0412 },
	{ "lt_LT", 0x0427 }, { "lt", 0x0427 },
	{ "lv_LV", 0x0426 }, { "lv", 0x0426 },
	{ "ms_MY", 0x043e }, { "ms", 0x043e },
	{ "nb_NO", 0x0414 }, { "nn_NO", 0x0814 }, { "no_NO", 0x0414 }, { "nb", 0x0414 },
	{ "nl_NL", 0x0413 }, { "nl_BE", 0x0813 }, { "nl", 0x0413 },
	{ "pl_PL", 0x0415 }, { "pl", 0x0415 },
	{ "pt_BR", 0x0416 }, { "pt_PT", 0x0816 }, { "pt", 0x0816 },
	{ "ro_RO", 0x0418 }, { "ro_MD", 0x0818 }, { "ro", 0x0418 },
	{ "ru_RU", 0x0419 }, { "ru_MD", 0x0819 }, { "ru", 0x0419 },
	{ "sk_SK", 0x041b }, { "sk", 0x041b },
	{ "sl_SI", 0x0424 }, { "sl", 0x0424 },
	{ "sr_RS", 0x241a }, { "sr_ME", 0x2c1a }, { "sr", 0x241a },
	{ "sv_SE", 0x041d }, { "sv_FI", 0x081d }, { "sv", 0x041d },
	{ "th_TH", 0x041e }, { "th", 0x041e },
	{ "tr_TR", 0x041f }, { "tr", 0x041f },
	{ "uk_UA", 0x0422 }, { "uk", 0x0422 },
	{ "vi_VN", 0x042a }, { "vi", 0x042a },
};

static const char* get_locale_env(void)
{
	const char* vars[] = { "LC_ALL", "LC_MESSAGES", "LANG", "LANGUAGE" };
	const char* v;
	size_t i;
	for (i = 0; i < ARRAYSIZE(vars); i++) {
		v = getenv(vars[i]);
		if (v != NULL && v[0] != 0 && strcmp(v, "C") != 0 && strncmp(v, "C.", 2) != 0 && strcmp(v, "POSIX") != 0)
			return v;
	}
	return "en_US";
}

uint16_t linux_locale_to_lcid(const char* locale)
{
	char name[16];
	size_t i, len;
	if (locale == NULL)
		locale = get_locale_env();
	/* Keep "ll_CC" (drop ".UTF-8", "@euro", ":fallbacks") */
	for (len = 0; len < sizeof(name) - 1 && locale[len] != 0 && strchr(".@:", locale[len]) == NULL; len++)
		name[len] = (locale[len] == '-') ? '_' : locale[len];
	name[len] = 0;
	for (i = 0; i < ARRAYSIZE(locale_table); i++)
		if (strcasecmp(locale_table[i].name, name) == 0)
			return locale_table[i].lcid;
	/* Try the language alone */
	if (len > 2 && name[2] == '_') {
		name[2] = 0;
		for (i = 0; i < ARRAYSIZE(locale_table); i++)
			if (strcasecmp(locale_table[i].name, name) == 0)
				return locale_table[i].lcid;
	}
	return 0x0409;
}

/* Returns the locale name in Windows format (e.g. "tr-TR") */
const char* linux_locale_name(void)
{
	static char name[16];
	const char* locale = get_locale_env();
	size_t len;
	for (len = 0; len < sizeof(name) - 1 && locale[len] != 0 && strchr(".@:", locale[len]) == NULL; len++)
		name[len] = (locale[len] == '_') ? '-' : locale[len];
	name[len] = 0;
	return name;
}

/* Windows LCID -> locale name (e.g. 0x041F -> "tr-TR"), see LCIDToLocaleName() */
char* ToLocaleName(DWORD lang_id)
{
	static char name[16];
	size_t i;
	for (i = 0; i < ARRAYSIZE(locale_table); i++) {
		/* Only consider full "ll_CC" entries */
		if (locale_table[i].lcid == (uint16_t)lang_id && strlen(locale_table[i].name) == 5) {
			snprintf(name, sizeof(name), "%c%c-%c%c", locale_table[i].name[0], locale_table[i].name[1],
				locale_table[i].name[3], locale_table[i].name[4]);
			return name;
		}
	}
	return "en-US";
}

LANGID GetSystemDefaultLangID(void)
{
	return (LANGID)linux_locale_to_lcid(NULL);
}

/*
 * Keyboard layout: XKB layout -> Windows KLID.
 * Ubuntu stores the system layout in /etc/default/keyboard.
 */
static const struct { const char* xkb; const char* klid; } keyboard_table[] = {
	{ "us", "00000409" }, { "gb", "00000809" }, { "uk", "00000809" }, { "ie", "00001809" },
	{ "de", "00000407" }, { "ch", "00000807" }, { "at", "00000407" }, { "fr", "0000040C" },
	{ "be", "0000080C" }, { "ca", "00001009" }, { "es", "0000040A" }, { "latam", "0000080A" },
	{ "it", "00000410" }, { "pt", "00000816" }, { "br", "00000416" }, { "nl", "00020409" },
	{ "dk", "00000406" }, { "no", "00000414" }, { "se", "0000041D" }, { "fi", "0000040B" },
	{ "is", "0000040F" }, { "pl", "00000415" }, { "cz", "00000405" }, { "sk", "0000041B" },
	{ "hu", "0000040E" }, { "si", "00000424" }, { "hr", "0000041A" }, { "rs", "0000081A" },
	{ "ro", "00010418" }, { "bg", "00000402" }, { "gr", "00000408" }, { "ru", "00000419" },
	{ "ua", "00000422" }, { "by", "00000423" }, { "tr", "0000041F" }, { "il", "0000040D" },
	{ "ara", "00000401" }, { "ir", "00000429" }, { "ee", "00000425" }, { "lv", "00000426" },
	{ "lt", "00000427" }, { "jp", "00000411" }, { "kr", "00000412" }, { "cn", "00000804" },
	{ "tw", "00000404" }, { "th", "0000041E" }, { "vn", "0000042A" }, { "in", "00004009" },
};

static BOOL read_xkb_layout(char* layout, size_t size)
{
	FILE* f = rufus_fopen("/etc/default/keyboard", "r");
	char line[256], *p, *e;
	BOOL r = FALSE;
	if (f == NULL)
		return FALSE;
	while (fgets(line, sizeof(line), f) != NULL) {
		if (strncmp(line, "XKBLAYOUT=", 10) != 0)
			continue;
		p = &line[10];
		if (*p == '"')
			p++;
		/* Only keep the first layout of a list such as "tr,us" */
		e = p + strcspn(p, "\",\n");
		*e = 0;
		snprintf(layout, size, "%s", p);
		r = (layout[0] != 0);
		break;
	}
	rufus_fclose(f);
	return r;
}

BOOL GetKeyboardLayoutNameA(LPSTR pwszKLID)
{
	char layout[32];
	size_t i;
	uint16_t lcid;
	if (read_xkb_layout(layout, sizeof(layout))) {
		for (i = 0; i < ARRAYSIZE(keyboard_table); i++) {
			if (strcmp(keyboard_table[i].xkb, layout) == 0) {
				strcpy(pwszKLID, keyboard_table[i].klid);
				return TRUE;
			}
		}
	}
	/* Default to the keyboard associated with the user's language */
	lcid = linux_locale_to_lcid(NULL);
	sprintf(pwszKLID, "%08X", lcid);
	return TRUE;
}

/* OEM (DOS) and ANSI code pages, per primary language */
static const struct { uint16_t lang; uint16_t oem; uint16_t ansi; } codepage_table[] = {
	{ 0x01, 720, 1256 },	/* Arabic */
	{ 0x02, 866, 1251 },	/* Bulgarian */
	{ 0x04, 936, 936 },	/* Chinese (Simplified; Traditional handled below) */
	{ 0x05, 852, 1250 },	/* Czech */
	{ 0x06, 850, 1252 },	/* Danish */
	{ 0x07, 850, 1252 },	/* German */
	{ 0x08, 737, 1253 },	/* Greek */
	{ 0x09, 437, 1252 },	/* English */
	{ 0x0a, 850, 1252 },	/* Spanish */
	{ 0x0b, 850, 1252 },	/* Finnish */
	{ 0x0c, 850, 1252 },	/* French */
	{ 0x0d, 862, 1255 },	/* Hebrew */
	{ 0x0e, 852, 1250 },	/* Hungarian */
	{ 0x0f, 850, 1252 },	/* Icelandic */
	{ 0x10, 850, 1252 },	/* Italian */
	{ 0x11, 932, 932 },	/* Japanese */
	{ 0x12, 949, 949 },	/* Korean */
	{ 0x13, 850, 1252 },	/* Dutch */
	{ 0x14, 850, 1252 },	/* Norwegian */
	{ 0x15, 852, 1250 },	/* Polish */
	{ 0x16, 850, 1252 },	/* Portuguese */
	{ 0x18, 852, 1250 },	/* Romanian */
	{ 0x19, 866, 1251 },	/* Russian */
	{ 0x1a, 852, 1250 },	/* Croatian / Serbian */
	{ 0x1b, 852, 1250 },	/* Slovak */
	{ 0x1d, 850, 1252 },	/* Swedish */
	{ 0x1e, 874, 874 },	/* Thai */
	{ 0x1f, 857, 1254 },	/* Turkish */
	{ 0x21, 437, 1252 },	/* Indonesian */
	{ 0x22, 866, 1251 },	/* Ukrainian */
	{ 0x23, 866, 1251 },	/* Belarusian */
	{ 0x24, 852, 1250 },	/* Slovenian */
	{ 0x25, 775, 1257 },	/* Estonian */
	{ 0x26, 775, 1257 },	/* Latvian */
	{ 0x27, 775, 1257 },	/* Lithuanian */
	{ 0x29, 720, 1256 },	/* Persian */
	{ 0x2a, 1258, 1258 },	/* Vietnamese */
	{ 0x3e, 850, 1252 },	/* Malay */
};

static const uint16_t* lookup_codepages(void)
{
	static const uint16_t def[3] = { 0x09, 437, 1252 };
	static const uint16_t big5[3] = { 0x04, 950, 950 };
	uint16_t lcid = linux_locale_to_lcid(NULL);
	size_t i;
	if (lcid == 0x0404 || lcid == 0x0c04 || lcid == 0x1404)
		return big5;
	for (i = 0; i < ARRAYSIZE(codepage_table); i++)
		if (codepage_table[i].lang == PRIMARYLANGID(lcid))
			return &codepage_table[i].lang;
	return def;
}

UINT GetOEMCP(void)
{
	return lookup_codepages()[1];
}

UINT GetACP(void)
{
	return lookup_codepages()[2];
}

/*
 * Time zone: IANA name -> Windows time zone key name.
 * Derived from the Unicode CLDR windowsZones.xml mappings (territory "001").
 */
static const struct { const char* iana; const char* windows; } tz_table[] = {
	{ "Etc/GMT+12", "Dateline Standard Time" }, { "Pacific/Pago_Pago", "UTC-11" },
	{ "Pacific/Honolulu", "Hawaiian Standard Time" }, { "America/Anchorage", "Alaskan Standard Time" },
	{ "America/Los_Angeles", "Pacific Standard Time" }, { "America/Tijuana", "Pacific Standard Time (Mexico)" },
	{ "America/Vancouver", "Pacific Standard Time" }, { "America/Phoenix", "US Mountain Standard Time" },
	{ "America/Denver", "Mountain Standard Time" }, { "America/Edmonton", "Mountain Standard Time" },
	{ "America/Chicago", "Central Standard Time" }, { "America/Winnipeg", "Central Standard Time" },
	{ "America/Mexico_City", "Central Standard Time (Mexico)" }, { "America/Regina", "Canada Central Standard Time" },
	{ "America/Guatemala", "Central America Standard Time" }, { "America/Bogota", "SA Pacific Standard Time" },
	{ "America/Lima", "SA Pacific Standard Time" }, { "America/New_York", "Eastern Standard Time" },
	{ "America/Toronto", "Eastern Standard Time" }, { "America/Detroit", "Eastern Standard Time" },
	{ "America/Havana", "Cuba Standard Time" }, { "America/Caracas", "Venezuela Standard Time" },
	{ "America/Halifax", "Atlantic Standard Time" }, { "America/La_Paz", "SA Western Standard Time" },
	{ "America/Santiago", "Pacific SA Standard Time" }, { "America/St_Johns", "Newfoundland Standard Time" },
	{ "America/Sao_Paulo", "E. South America Standard Time" }, { "America/Argentina/Buenos_Aires", "Argentina Standard Time" },
	{ "America/Buenos_Aires", "Argentina Standard Time" }, { "America/Montevideo", "Montevideo Standard Time" },
	{ "America/Godthab", "Greenland Standard Time" }, { "America/Nuuk", "Greenland Standard Time" },
	{ "Atlantic/South_Georgia", "UTC-02" }, { "Atlantic/Azores", "Azores Standard Time" },
	{ "Atlantic/Cape_Verde", "Cape Verde Standard Time" }, { "Etc/UTC", "UTC" }, { "Etc/GMT", "UTC" }, { "UTC", "UTC" },
	{ "Europe/London", "GMT Standard Time" }, { "Europe/Dublin", "GMT Standard Time" }, { "Europe/Lisbon", "GMT Standard Time" },
	{ "Atlantic/Reykjavik", "Greenwich Standard Time" }, { "Africa/Casablanca", "Morocco Standard Time" },
	{ "Europe/Berlin", "W. Europe Standard Time" }, { "Europe/Amsterdam", "W. Europe Standard Time" },
	{ "Europe/Rome", "W. Europe Standard Time" }, { "Europe/Stockholm", "W. Europe Standard Time" },
	{ "Europe/Vienna", "W. Europe Standard Time" }, { "Europe/Zurich", "W. Europe Standard Time" },
	{ "Europe/Oslo", "W. Europe Standard Time" }, { "Europe/Luxembourg", "W. Europe Standard Time" },
	{ "Europe/Budapest", "Central Europe Standard Time" }, { "Europe/Prague", "Central Europe Standard Time" },
	{ "Europe/Bratislava", "Central Europe Standard Time" }, { "Europe/Belgrade", "Central Europe Standard Time" },
	{ "Europe/Ljubljana", "Central Europe Standard Time" }, { "Europe/Paris", "Romance Standard Time" },
	{ "Europe/Brussels", "Romance Standard Time" }, { "Europe/Madrid", "Romance Standard Time" },
	{ "Europe/Copenhagen", "Romance Standard Time" }, { "Europe/Warsaw", "Central European Standard Time" },
	{ "Europe/Zagreb", "Central European Standard Time" }, { "Europe/Sarajevo", "Central European Standard Time" },
	{ "Africa/Lagos", "W. Central Africa Standard Time" }, { "Europe/Athens", "GTB Standard Time" },
	{ "Europe/Bucharest", "GTB Standard Time" }, { "Asia/Beirut", "Middle East Standard Time" },
	{ "Africa/Cairo", "Egypt Standard Time" }, { "Europe/Chisinau", "E. Europe Standard Time" },
	{ "Asia/Damascus", "Syria Standard Time" }, { "Asia/Gaza", "West Bank Standard Time" },
	{ "Africa/Johannesburg", "South Africa Standard Time" }, { "Europe/Kiev", "FLE Standard Time" },
	{ "Europe/Kyiv", "FLE Standard Time" }, { "Europe/Helsinki", "FLE Standard Time" }, { "Europe/Riga", "FLE Standard Time" },
	{ "Europe/Sofia", "FLE Standard Time" }, { "Europe/Tallinn", "FLE Standard Time" }, { "Europe/Vilnius", "FLE Standard Time" },
	{ "Asia/Jerusalem", "Israel Standard Time" }, { "Europe/Kaliningrad", "Kaliningrad Standard Time" },
	{ "Africa/Tripoli", "Libya Standard Time" }, { "Asia/Amman", "Jordan Standard Time" },
	{ "Asia/Baghdad", "Arabic Standard Time" }, { "Europe/Istanbul", "Turkey Standard Time" },
	{ "Asia/Istanbul", "Turkey Standard Time" }, { "Asia/Riyadh", "Arab Standard Time" },
	{ "Asia/Kuwait", "Arab Standard Time" }, { "Europe/Minsk", "Belarus Standard Time" },
	{ "Europe/Moscow", "Russian Standard Time" }, { "Africa/Nairobi", "E. Africa Standard Time" },
	{ "Asia/Tehran", "Iran Standard Time" }, { "Asia/Dubai", "Arabian Standard Time" },
	{ "Europe/Samara", "Russia Time Zone 3" }, { "Asia/Baku", "Azerbaijan Standard Time" },
	{ "Asia/Tbilisi", "Georgian Standard Time" }, { "Asia/Yerevan", "Caucasus Standard Time" },
	{ "Asia/Kabul", "Afghanistan Standard Time" }, { "Asia/Yekaterinburg", "Ekaterinburg Standard Time" },
	{ "Asia/Karachi", "Pakistan Standard Time" }, { "Asia/Tashkent", "West Asia Standard Time" },
	{ "Asia/Kolkata", "India Standard Time" }, { "Asia/Calcutta", "India Standard Time" },
	{ "Asia/Colombo", "Sri Lanka Standard Time" }, { "Asia/Kathmandu", "Nepal Standard Time" },
	{ "Asia/Almaty", "Central Asia Standard Time" }, { "Asia/Dhaka", "Bangladesh Standard Time" },
	{ "Asia/Omsk", "Omsk Standard Time" }, { "Asia/Yangon", "Myanmar Standard Time" },
	{ "Asia/Bangkok", "SE Asia Standard Time" }, { "Asia/Jakarta", "SE Asia Standard Time" },
	{ "Asia/Ho_Chi_Minh", "SE Asia Standard Time" }, { "Asia/Novosibirsk", "N. Central Asia Standard Time" },
	{ "Asia/Krasnoyarsk", "North Asia Standard Time" }, { "Asia/Shanghai", "China Standard Time" },
	{ "Asia/Hong_Kong", "China Standard Time" }, { "Asia/Irkutsk", "North Asia East Standard Time" },
	{ "Asia/Singapore", "Singapore Standard Time" }, { "Asia/Kuala_Lumpur", "Singapore Standard Time" },
	{ "Asia/Manila", "Singapore Standard Time" }, { "Australia/Perth", "W. Australia Standard Time" },
	{ "Asia/Taipei", "Taipei Standard Time" }, { "Asia/Ulaanbaatar", "Ulaanbaatar Standard Time" },
	{ "Asia/Tokyo", "Tokyo Standard Time" }, { "Asia/Seoul", "Korea Standard Time" },
	{ "Asia/Pyongyang", "North Korea Standard Time" }, { "Asia/Yakutsk", "Yakutsk Standard Time" },
	{ "Australia/Adelaide", "Cen. Australia Standard Time" }, { "Australia/Darwin", "AUS Central Standard Time" },
	{ "Australia/Brisbane", "E. Australia Standard Time" }, { "Australia/Sydney", "AUS Eastern Standard Time" },
	{ "Australia/Melbourne", "AUS Eastern Standard Time" }, { "Pacific/Port_Moresby", "West Pacific Standard Time" },
	{ "Australia/Hobart", "Tasmania Standard Time" }, { "Asia/Vladivostok", "Vladivostok Standard Time" },
	{ "Asia/Magadan", "Magadan Standard Time" }, { "Pacific/Guadalcanal", "Central Pacific Standard Time" },
	{ "Pacific/Auckland", "New Zealand Standard Time" }, { "Pacific/Fiji", "Fiji Standard Time" },
	{ "Asia/Kamchatka", "Russia Time Zone 11" }, { "Pacific/Tongatapu", "Tonga Standard Time" },
	{ "Pacific/Apia", "Samoa Standard Time" }, { "Pacific/Kiritimati", "Line Islands Standard Time" },
};

static BOOL get_iana_timezone(char* name, size_t size)
{
	char link[MAX_PATH], *p;
	ssize_t r;
	FILE* f;
	const char* tz = getenv("TZ");
	if (tz != NULL && tz[0] != 0) {
		snprintf(name, size, "%s", (tz[0] == ':') ? &tz[1] : tz);
		return TRUE;
	}
	r = readlink("/etc/localtime", link, sizeof(link) - 1);
	if (r > 0) {
		link[r] = 0;
		p = strstr(link, "zoneinfo/");
		if (p != NULL) {
			snprintf(name, size, "%s", p + 9);
			return TRUE;
		}
	}
	f = rufus_fopen("/etc/timezone", "r");
	if (f != NULL) {
		BOOL ok = (fgets(name, (int)size, f) != NULL);
		rufus_fclose(f);
		if (ok) {
			name[strcspn(name, "\r\n")] = 0;
			return (name[0] != 0);
		}
	}
	return FALSE;
}

DWORD GetDynamicTimeZoneInformation(PDYNAMIC_TIME_ZONE_INFORMATION tzi)
{
	char iana[128];
	const char* windows = NULL;
	size_t i;
	if (tzi == NULL)
		return TIME_ZONE_ID_INVALID;
	memset(tzi, 0, sizeof(*tzi));
	if (get_iana_timezone(iana, sizeof(iana))) {
		for (i = 0; i < ARRAYSIZE(tz_table); i++) {
			if (strcmp(tz_table[i].iana, iana) == 0) {
				windows = tz_table[i].windows;
				break;
			}
		}
		if (windows == NULL)
			uprintf("NOTE: No Windows equivalent known for time zone '%s' - using UTC", iana);
	}
	if (windows == NULL)
		windows = "UTC";
	MultiByteToWideChar(CP_UTF8, 0, windows, -1, tzi->TimeZoneKeyName, ARRAYSIZE(tzi->TimeZoneKeyName));
	return TIME_ZONE_ID_UNKNOWN;
}

/* The user who launched us through pkexec or sudo */
static uid_t invoking_uid(void)
{
	const char* vars[] = { "PKEXEC_UID", "SUDO_UID" };
	const char* v;
	size_t i;
	for (i = 0; i < ARRAYSIZE(vars); i++) {
		v = getenv(vars[i]);
		if (v != NULL && v[0] != 0)
			return (uid_t)strtoul(v, NULL, 10);
	}
	return getuid();
}

void linux_invoking_user_ids(uid_t* uid, gid_t* gid)
{
	struct passwd* pw = getpwuid(invoking_uid());
	if (uid != NULL)
		*uid = (pw == NULL) ? invoking_uid() : pw->pw_uid;
	if (gid != NULL)
		*gid = (pw == NULL) ? getgid() : pw->pw_gid;
}

const char* linux_invoking_user_name(void)
{
	static char name[128];
	struct passwd* pw = getpwuid(invoking_uid());
	snprintf(name, sizeof(name), "%s", (pw == NULL || pw->pw_name == NULL) ? "User" : pw->pw_name);
	return name;
}

const char* linux_invoking_user_home(void)
{
	static char home[MAX_PATH];
	struct passwd* pw = getpwuid(invoking_uid());
	const char* h = (pw == NULL) ? getenv("HOME") : pw->pw_dir;
	snprintf(home, sizeof(home), "%s", (h == NULL) ? "/tmp" : h);
	return home;
}
