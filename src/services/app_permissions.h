/**
 * Local app permission labels and user-facing access summaries.
 */
#ifndef LZ_APP_PERMISSIONS_H
#define LZ_APP_PERMISSIONS_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

const char *lz_app_permission_name(uint16_t bit);
const char *lz_app_permission_prompt(uint16_t bit);
uint16_t    lz_app_permission_known_mask(void);
void        lz_app_permissions_list(uint16_t perms, char *out, size_t cap);
void        lz_app_permissions_summary(uint16_t perms, char *out, size_t cap);

#ifdef __cplusplus
}
#endif

#endif
