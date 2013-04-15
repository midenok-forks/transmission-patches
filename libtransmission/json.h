/*
 * This file Copyright (C) Mnemosyne LLC
 *
 * This file is licensed by the GPL version 2. Works owned by the
 * Transmission project are granted a special exemption to clause 2 (b)
 * so that the bulk of its code can remain under the MIT license.
 * This exemption does not extend to derived works not owned by
 * the Transmission project.
 *
 * $Id: json.h 13625 2012-12-05 17:29:46Z jordan $
 */

#ifndef TR_JSON_H

#ifdef __cplusplus
extern "C" {
#endif

/** @ingroup tr_benc */
int tr_jsonParse (const char * source, /* Such as a filename. Only when logging an error */
                  const void *     vbuf,
                  size_t           len,
                  struct tr_benc * setme_benc,
                  const uint8_t ** setme_end);

#ifdef __cplusplus
}
#endif

#endif
