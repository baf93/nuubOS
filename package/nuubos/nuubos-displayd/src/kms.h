#ifndef NUUBOS_KMS_H
#define NUUBOS_KMS_H

#include <stdbool.h>
#include <stddef.h>

int nuubos_kms_apply(bool use_hdmi, char *mode_desc, size_t mode_desc_size);

#endif
