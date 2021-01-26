/* Copyright (c) 2018 Hans-Kristian Arntzen
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#pragma once

#ifdef ANDROID
#include <android/log.h>
#define LOGI(...) do { \
	__android_log_print(ANDROID_LOG_INFO, "Fossilize", __VA_ARGS__); \
	fprintf(stderr, "Fossilize INFO: " __VA_ARGS__); \
} while(0)
#define LOGW(...) do { \
	__android_log_print(ANDROID_LOG_WARN, "Fossilize", __VA_ARGS__); \
	fprintf(stderr, "Fossilize WARN: " __VA_ARGS__); \
} while(0)
#define LOGE(...) do { \
	__android_log_print(ANDROID_LOG_ERROR, "Fossilize", __VA_ARGS__); \
	fprintf(stderr, "Fossilize ERROR: " __VA_ARGS__); \
} while(0)
#else
#include <stdio.h>
#define LOGI(...) do { fprintf(stderr, "Fossilize INFO: " __VA_ARGS__); fflush(stderr); } while(0)
#define LOGW(...) do { fprintf(stderr, "Fossilize WARN: " __VA_ARGS__); fflush(stderr); } while(0)
#define LOGE(...) do { fprintf(stderr, "Fossilize ERROR: " __VA_ARGS__); fflush(stderr); } while(0)
#endif
