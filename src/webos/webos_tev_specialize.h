/**
 * @file webos_tev_specialize.h
 * @brief Straight-line GLSL for the port's TEV shader variants (see the .c file).
 */
#ifndef ACWEBOS_TEV_SPECIALIZE_H
#define ACWEBOS_TEV_SPECIALIZE_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * If @p source is a specialised TEV fragment shader variant (GLSL ES, after
 * the version rewrite), returns a malloc'd copy whose main() has every
 * configuration choice resolved. Returns NULL for any other source, or if the
 * variant cannot be specialised; the caller then uses @p source unchanged.
 */
char *webos_tev_specialize(const char *source);

#ifdef __cplusplus
}
#endif

#endif /* ACWEBOS_TEV_SPECIALIZE_H */
