#pragma once

#include <cstddef>

// Encrypted log files (0.5.7). Only the holder of the private key can read
// them; the DLL carries the public key alone.
//
// File layout (*.cslog):
//   "CSLOG\0\1\0"      8 bytes magic / version 1
//   uint16 LE n        length of the wrapped key (256 for RSA-2048)
//   n bytes            RSA-OAEP(SHA-256) of key[32] || iv[16]
//   ...                the log text, AES-256-CTR (counter = iv, 128-bit
//                      big-endian increment), written as it happens -- a
//                      file cut off by a crash still decrypts up to there.
//
// Read them with tools/cslog_decrypt.py and the private key.
//
// Not thread-safe per file: every caller already writes under its own lock.
struct CoSyncCryptLog;

// Creates (truncates) the file and writes the header. nullptr if the
// encryption could not be set up -- then nothing is written at all, never
// plain text.
CoSyncCryptLog* CoSyncCryptLog_Open(const char* path);
// Encrypts and appends, then flushes.
void CoSyncCryptLog_Write(CoSyncCryptLog* log, const char* data, size_t len);
