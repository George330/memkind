/*
 * Copyright (c) 2018 - 2019 Intel Corporation
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in
 *       the documentation and/or other materials provided with the
 *       distribution.
 *
 *     * Neither the name of Intel Corporation nor the names of its
 *       contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY LOG OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <memkind.h>

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>

#define MB (1024 * 1024)

static char path[PATH_MAX]="/tmp/";

static void print_err_message(int err)
{
    char error_message[MEMKIND_ERROR_MESSAGE_SIZE];
    memkind_error_message(err, error_message, MEMKIND_ERROR_MESSAGE_SIZE);
    fprintf(stderr, "%s\n", error_message);
}

int main(int argc, char *argv[])
{
    struct memkind *pmem_kind_unlimited = NULL;
    int err = 0;

    if (argc > 2) {
        fprintf(stderr, "Usage: %s [pmem_kind_dir_path]\n", argv[0]);
        return 1;
    } else if (argc == 2 && (realpath(argv[1], path) == NULL)) {
        fprintf(stderr, "Incorrect pmem_kind_dir_path %s\n", argv[1]);
        return 1;
    }

    fprintf(stdout,
            "This example shows difference between the expected and the actual allocation size."
            "\nPMEM kind directory: %s\n", path);

    err = memkind_create_pmem(path, 0, &pmem_kind_unlimited);
    if (err) {
        print_err_message(err);
        return 1;
    }

    char *pmem_str10 = NULL;
    char *pmem_str11 = NULL;
    char *pmem_str12 = NULL;

    // 32 bytes allocation
    pmem_str10 = (char *)memkind_malloc(pmem_kind_unlimited, 32);
    if (pmem_str10 == NULL) {
        fprintf(stderr, "Unable to allocate pmem string (pmem_str10).\n");
        return 1;
    }

    // Check real usable size for this allocation
    if (memkind_malloc_usable_size(pmem_kind_unlimited, pmem_str10) != 32) {
        fprintf(stderr, "Wrong usable size for small allocation (pmem_str10).\n");
        return 1;
    }

    // 31 bytes allocation
    pmem_str11 = (char *)memkind_malloc(pmem_kind_unlimited, 31);
    if (pmem_str11 == NULL) {
        fprintf(stderr, "Unable to allocate pmem string (pmem_str11).\n");
        return 1;
    }

    // Check real usable size for this allocation, its 32 again
    if (memkind_malloc_usable_size(pmem_kind_unlimited, pmem_str11) != 32) {
        fprintf(stderr, "Wrong usable size for small allocation (pmem_str11).\n");
        return 1;
    }

    // 33 bytes allocation
    pmem_str12 = (char *)memkind_malloc(pmem_kind_unlimited, 33);
    if (pmem_str12 == NULL) {
        fprintf(stderr, "Unable to allocate pmem string (pmem_str12).\n");
        return 1;
    }

    // Check real usable size for this allocation, its 48 now
    if (memkind_malloc_usable_size(pmem_kind_unlimited, pmem_str12) != 48) {
        fprintf(stderr, "Wrong usable size for small allocation (pmem_str12).\n");
        return 1;
    }

    memkind_free(pmem_kind_unlimited, pmem_str10);
    memkind_free(pmem_kind_unlimited, pmem_str11);
    memkind_free(pmem_kind_unlimited, pmem_str12);

    // 5MB allocation
    pmem_str10 = (char *)memkind_malloc(pmem_kind_unlimited, 5 * MB);
    if (pmem_str10 == NULL) {
        fprintf(stderr, "Unable to allocate pmem string (pmem_str10).\n");
        return 1;
    }

    // Check real usable size for this allocation
    if (memkind_malloc_usable_size(pmem_kind_unlimited, pmem_str10) != 5 * MB) {
        fprintf(stderr, "Wrong usable size for large allocation (pmem_str10).\n");
        return 1;
    }

    // 5MB + 1B allocation
    pmem_str11 = (char *)memkind_malloc(pmem_kind_unlimited, 5 * MB + 1);
    if (pmem_str11 == NULL) {
        fprintf(stderr, "Unable to allocate pmem string (pmem_str11).\n");
        return 1;
    }

    // Check real usable size for this allocation, its 6MB now
    if (memkind_malloc_usable_size(pmem_kind_unlimited, pmem_str11) != 6 * MB) {
        fprintf(stderr, "Wrong usable size for large allocation (pmem_str11).\n");
        return 1;
    }

    memkind_free(pmem_kind_unlimited, pmem_str10);
    memkind_free(pmem_kind_unlimited, pmem_str11);

    err = memkind_destroy_kind(pmem_kind_unlimited);
    if (err) {
        print_err_message(err);
        return 1;
    }

    fprintf(stdout,
            "The real size of the allocation has been successfully read.\n");

    return 0;
}
