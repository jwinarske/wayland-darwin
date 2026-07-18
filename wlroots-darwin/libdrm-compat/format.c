/*
 * libdrm-compat: DRM format-name helpers.
 *
 * Reimplementations of libdrm's drmGetFormatName, drmGetFormatModifierVendor,
 * and drmGetFormatModifierName, sufficient for wlroots' logging on Darwin.
 * drmGetFormatName matches libdrm exactly. The Darwin renderer path is
 * LINEAR-only, so the deep per-vendor modifier decoders in real libdrm are not
 * reproduced; named simple modifiers (LINEAR/INVALID) are handled and any other
 * modifier is rendered as "<VENDOR>_0x<value>" (or a bare hex if the vendor is
 * unknown), which is a stable, human-readable fallback.
 *
 * These are the only libdrm symbols with real behavior in the shim; everything
 * else is a behavioral stub (stubs.c).
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <drm_fourcc.h>

char *
drmGetFormatName(uint32_t format)
{
	const char *be;
	char code[5];
	char *str;
	size_t size, i;

	be = (format & DRM_FORMAT_BIG_ENDIAN) ? "_BE" : "";
	format &= ~DRM_FORMAT_BIG_ENDIAN;

	if (format == DRM_FORMAT_INVALID)
		return strdup("INVALID");

	code[0] = (char)((format >> 0) & 0xFF);
	code[1] = (char)((format >> 8) & 0xFF);
	code[2] = (char)((format >> 16) & 0xFF);
	code[3] = (char)((format >> 24) & 0xFF);
	code[4] = '\0';

	/* Trim trailing spaces. */
	for (i = 3; i > 0 && code[i] == ' '; i--)
		code[i] = '\0';

	size = strlen(code) + strlen(be) + 1;
	str = malloc(size);
	if (!str)
		return NULL;
	snprintf(str, size, "%s%s", code, be);
	return str;
}

static const struct {
	uint8_t vendor;
	const char *name;
} vendor_table[] = {
	{ DRM_FORMAT_MOD_VENDOR_NONE,      "NONE" },
	{ DRM_FORMAT_MOD_VENDOR_INTEL,     "INTEL" },
	{ DRM_FORMAT_MOD_VENDOR_AMD,       "AMD" },
	{ DRM_FORMAT_MOD_VENDOR_NVIDIA,    "NVIDIA" },
	{ DRM_FORMAT_MOD_VENDOR_SAMSUNG,   "SAMSUNG" },
	{ DRM_FORMAT_MOD_VENDOR_QCOM,      "QCOM" },
	{ DRM_FORMAT_MOD_VENDOR_VIVANTE,   "VIVANTE" },
	{ DRM_FORMAT_MOD_VENDOR_BROADCOM,  "BROADCOM" },
	{ DRM_FORMAT_MOD_VENDOR_ARM,       "ARM" },
	{ DRM_FORMAT_MOD_VENDOR_ALLWINNER, "ALLWINNER" },
	{ DRM_FORMAT_MOD_VENDOR_AMLOGIC,   "AMLOGIC" },
};

char *
drmGetFormatModifierVendor(uint64_t modifier)
{
	uint8_t vendor = fourcc_mod_get_vendor(modifier);
	size_t i;

	for (i = 0; i < sizeof(vendor_table) / sizeof(vendor_table[0]); i++) {
		if (vendor_table[i].vendor == vendor)
			return strdup(vendor_table[i].name);
	}
	return NULL;
}

char *
drmGetFormatModifierName(uint64_t modifier)
{
	unsigned long long value;
	char *vendor;
	char buf[64];

	if (modifier == DRM_FORMAT_MOD_INVALID)
		return strdup("INVALID");
	if (modifier == DRM_FORMAT_MOD_LINEAR)
		return strdup("LINEAR");

	vendor = drmGetFormatModifierVendor(modifier);
	value = (unsigned long long)(modifier & 0x00ffffffffffffffULL);
	if (vendor) {
		snprintf(buf, sizeof buf, "%s_0x%llx", vendor, value);
		free(vendor);
	} else {
		snprintf(buf, sizeof buf, "0x%016llx", (unsigned long long)modifier);
	}
	return strdup(buf);
}
