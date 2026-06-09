/*
 * js8_freetext.h — JS8 free-text alphabet sanitization (L7.11g.7-fix6)
 * =============================================================================
 * Why this exists
 * ---------------
 * The JS8 protocol packs free-text bodies using a Huffman codebook (see
 * `static const std::map<std::string, std::string> hufftable` in
 * `nanojs8_gfsk8/src/Varicode.cpp`, lines 342-385). Stock JS8Call also has a
 * fallback dictionary compressor (JSC::compress) that handles a wider char
 * set, but in NanoJS8 that function is **stubbed** for flash savings (see
 * `nanojs8_gfsk8/src/JSC.cpp`, ATTRIBUTION.md). With the compress path dead,
 * Huffman is our only data-frame path — and `packHuffMessage` is
 * **all-or-nothing**:
 *
 *     for (char ch : input) {
 *         if (!validChars.count(toupper(ch))) {
 *             *n = 0;
 *             return frame;          // entire input rejected
 *         }
 *     }
 *
 * Any single char outside the Huffman alphabet → 0 chars packed → the
 * `buildMessageFrames` safety guard added in fix3 breaks the loop cleanly →
 * only the directed header frame is transmitted → recipient sees an empty
 * MSG body and (in stock JS8Call) auto-ACKs it as a ping → we mark
 * STORE→DELIVERED off a false ACK while the body never reached the air.
 *
 * On-air repro Jun 7 2026 (L7.11g.7-fix5): mailbox id=9, body
 * "MULTI MSG TES #1". '#' is not in the Huffman codebook, so the entire
 * "MULTI MSG TES #1" body was rejected, only "KD8PGB MSG 9" went out, KD8PGB
 * auto-ACK'd, we marked DELIVERED.
 *
 * What this helper does
 * ---------------------
 * Walks a writable C string in place:
 *   - Uppercases lowercase ASCII letters (the Huff codebook is uppercase-only)
 *   - Replaces every char NOT in the Huffman alphabet with **space**
 *
 * The space substitution was chosen by the operator (W5DMH) as the least
 * intrusive — invalid punctuation becomes a non-printing gap rather than a
 * visible '.' marker. Operator-readable text is preserved verbatim; only the
 * forbidden chars vanish.
 *
 * The valid alphabet is the **explicit char set** from hufftable in
 * `Varicode.cpp`:
 *
 *     space, A B C D E F G H I J K L M N O P Q R S T U V W X Y Z
 *            0 1 2 3 4 5 6 7 8 9 . - + ? ! " /
 *
 * That's 44 chars. Notable absences (any of these → space):
 *   #  ,  :  ;  @  *  (  )  [  ]  {  }  <  >  &  %  $  ~  ^  =  '  `  _  |  \
 *   tab, newline, all extended-ASCII / UTF-8 bytes
 *
 * Reliability notes
 * -----------------
 * - The function is idempotent — running it twice on the same buffer is a
 *   no-op.
 * - The string length never changes (substitution, not deletion), so caller
 *   buffer sizing is unaffected. snprintf() truncation behaviour is
 *   unchanged.
 * - The caller is responsible for logging WARN if `nanojs8_js8_freetext_sanitize`
 *   returns a non-zero replacement count — operators need to know their
 *   text was modified.
 * - Defensive at TX boundary only (operator preference Jun 7 2026): the
 *   mailbox NVS keeps the operator's original bytes for INBOX display;
 *   sanitization is applied at the wire-construction site. This means the
 *   INBOX can show `#` and other punctuation faithfully while still
 *   producing transmittable wire text downstream.
 *
 * License: GPL-3.0
 */
#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Sanitize a free-text body in place for JS8 Huffman packing.
 *
 * @param s  Writable, NUL-terminated string. May be NULL (treated as empty).
 *
 * @return Number of characters that were modified. Zero means the input was
 *         already valid; caller may skip the WARN log in that case.
 *
 * The string is modified IN PLACE. Length is preserved (every replacement is
 * single-char → single-char). NUL terminator is preserved.
 */
size_t nanojs8_js8_freetext_sanitize(char *s);

/**
 * Read-only check: scan a string for any chars outside the JS8 Huffman
 * alphabet without modifying it. Useful for defensive assertions at TX
 * boundaries (e.g. tx_queue can ESP_LOGE if a wire arrived dirty after
 * upstream sanitization should have cleaned it).
 *
 * @param s  NUL-terminated string. May be NULL (treated as empty).
 *
 * @return Number of out-of-alphabet characters found. Zero means clean.
 */
size_t nanojs8_js8_freetext_count_invalid(const char *s);

#ifdef __cplusplus
}
#endif
