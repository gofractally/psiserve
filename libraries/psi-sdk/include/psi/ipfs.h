#ifndef PSI_IPFS_H
#define PSI_IPFS_H

#include <psi/host.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Store data in the content-addressed store.
 * Writes the CID string (base32-lower multibase, e.g. "bafy...") to cid_out.
 * Returns CID string length on success, or negative PsiError. */
__attribute__((import_module("psi"), import_name("ipfs_put")))
int psi_ipfs_put(const void* data, int data_len, void* cid_out, int cid_cap);

/* Read data by CID string.
 * Returns bytes written to buf, 0 on EOF, or negative PsiError. */
__attribute__((import_module("psi"), import_name("ipfs_get")))
int psi_ipfs_get(const void* cid, int cid_len, long long offset,
                 void* buf, int buf_len);

/* Get content size by CID string.
 * Writes uint64_t (8 bytes) to size_out.
 * Returns 0 on success, or negative PsiError. */
__attribute__((import_module("psi"), import_name("ipfs_stat")))
int psi_ipfs_stat(const void* cid, int cid_len, void* size_out);

#ifdef __cplusplus
}
#endif

#endif /* PSI_IPFS_H */
