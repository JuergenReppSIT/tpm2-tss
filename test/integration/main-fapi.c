/* SPDX-License-Identifier: BSD-2-Clause */
/*******************************************************************************
 * Copyright 2017, Fraunhofer SIT sponsored by Infineon Technologies AG
 * All rights reserved.
 *******************************************************************************/

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stdio.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <json-c/json_util.h>
#include <json-c/json_tokener.h>

#include "tss2_esys.h"
#include "tss2_fapi.h"
#include "test-fapi.h"
#include "fapi_int.h"

#define LOGDEFAULT LOGLEVEL_INFO
#define LOGMODULE test
#include "util/log.h"
#include "util/aux_util.h"

#include "test-common.h"

char *fapi_profile = NULL;
TSS2_TEST_FAPI_CONTEXT *fapi_test_ctx = NULL;

struct tpm_state {
    TPMS_CAPABILITY_DATA capabilities[7];
};

bool file_exists (char *path) {
  struct stat   buffer;
  return (stat (path, &buffer) == 0);
}

/* Determine integer number from json object. */
static int64_t
get_number(json_object *jso) {
    const char* token;
    int itoken = 0;
    int pos = 0;
    int64_t num;

    token = json_object_get_string(jso);
    if (strncmp(token, "0x", 2) == 0) {
        itoken = 2;
        sscanf(&token[itoken], "%"PRIx64"%n", &num, &pos);
    } else {
        sscanf(&token[itoken], "%"PRId64"%n", &num, &pos);
    }
    return num;
}

/* Determin number of fields in a json objecd. */
size_t nmb_of_fields(json_object *jso) {
    size_t n = 0;
    json_object_object_foreach(jso, key, val) {
        UNUSED(val);
        UNUSED(key);
        n++;
    }
    return n;
}

/* Compare two json objects.
 *
 * Only strings, integers, array and json objects are supported.
 */
bool cmp_jso(json_object *jso1, json_object *jso2) {
    enum json_type type1, type2;
    size_t i, size;
    type1 = json_object_get_type(jso1);
    type2 = json_object_get_type(jso2);
    if (type1 != type2) {
        return false;
    }
    if (type1 == json_type_object) {
        if (nmb_of_fields(jso1) != nmb_of_fields(jso2)) {
            return false;
        }
        json_object_object_foreach(jso1, key1, jso_sub1) {
            json_object *jso_sub2;
            if (!json_object_object_get_ex(jso2, key1, &jso_sub2)) {
                return false;
            }
            if (!cmp_jso(jso_sub1, jso_sub2)) {
                    return false;
            }
        }
        return true;
    } else if (type1 == json_type_int) {
        return (get_number(jso1) == get_number(jso2));
    } else if (type1 == json_type_array) {
        size = json_object_array_length(jso1);
        /* Cast to size_t due to change in json-c API.
           older versions use result type int */
        if (size != (size_t)json_object_array_length(jso2)) {
            return false;
        }
        for (i = 0; i < size; i++) {
            if (!cmp_jso(json_object_array_get_idx(jso1, i),
                         json_object_array_get_idx(jso2, i))) {
                return false;
            }
        }
        return true;
    } else if (type1 == json_type_string) {
        return (strcmp(json_object_get_string(jso1),
                       json_object_get_string(jso2)) == 0);
    } else {
        return false;
    }
}

/* Compare two delimter sparated token lists. */
bool cmp_strtokens(char* string1, char *string2, char *delimiter) {
    bool found = false;
    char *token1 = NULL;
    char *token2 = NULL;
    char *end_token1;
    char *end_token2;
    char *string2_copy;

    string1 = strdup(string1);
    ASSERT(string1);
    token1 = strtok_r(string1, delimiter, &end_token1);
    while(token1 != NULL) {
        found = false;
        string2_copy = strdup(string2);
        ASSERT(string2_copy);
        token2 = strtok_r(string2_copy, delimiter, &end_token2);
        while (token2 != NULL) {
            if (strcmp(token1, token2) == 0) {
                found = true;
                break;
            }
            token2 = strtok_r(NULL, delimiter, &end_token2);
        }
        free(string2_copy);
        if (!found) {
            break;
        }
        token1 = strtok_r(NULL, delimiter, &end_token1);
    }
    free(string1);
    return found;

 error:
    SAFE_FREE(string1);
    return false;
}

TSS2_RC
pcr_reset(FAPI_CONTEXT *context, UINT32 pcr)
{
    TSS2_RC r;
    TSS2_TCTI_CONTEXT *tcti;
    ESYS_CONTEXT *esys;

    r = Fapi_GetTcti(context, &tcti);
    goto_if_error(r, "Error Fapi_GetTcti", error);

    r = Esys_Initialize(&esys, tcti, NULL);
    goto_if_error(r, "Error Fapi_GetTcti", error);

    r = Esys_PCR_Reset(esys, pcr,
                       ESYS_TR_PASSWORD, ESYS_TR_NONE, ESYS_TR_NONE);
    Esys_Finalize(&esys);
    goto_if_error(r, "Error Eys_PCR_Reset", error);

error:
    return r;
}

TSS2_RC
pcr_extend(FAPI_CONTEXT *context, UINT32 pcr, TPML_DIGEST_VALUES *digest_values)
{
    TSS2_RC r;
    TSS2_TCTI_CONTEXT *tcti;
    ESYS_CONTEXT *esys;

    r = Fapi_GetTcti(context, &tcti);
    goto_if_error(r, "Error Fapi_GetTcti", error);

    r = Esys_Initialize(&esys, tcti, NULL);
    goto_if_error(r, "Error Fapi_GetTcti", error);

    r = Esys_PCR_Extend(esys, pcr,
                        ESYS_TR_PASSWORD, ESYS_TR_NONE, ESYS_TR_NONE,
                        digest_values);
    Esys_Finalize(&esys);
    goto_if_error(r, "Error Eys_PCR_Reset", error);

error:
    return r;
}

int init_fapi(char *profile, FAPI_CONTEXT **fapi_context)
{
    TSS2_RC rc;
    int ret, size;
    char *config = NULL;
    char *config_path = NULL;
    char *config_env = NULL;
    char *config_bak = NULL;
    FILE *config_file;
    char *tmpdir;

    fapi_profile = profile;
    tmpdir = fapi_test_ctx->tmpdir;

    /* First we construct a fapi config file */
#if defined(FAPI_NONTPM)
    size = asprintf(&config, "{\n"
                    "     \"profile_name\": \"%s\",\n"
                    "     \"profile_dir\": \"" TOP_SOURCEDIR "/test/data/fapi/\",\n"
                    "     \"user_dir\": \"%s/user/dir\",\n"
                    "     \"system_dir\": \"%s/system_dir\",\n"
                    "     \"system_pcrs\" : [],\n"
                    "     \"log_dir\" : \"%s\",\n"
                    "     \"tcti\": \"none\",\n"
                    "}\n",
                    profile, tmpdir, tmpdir, tmpdir);
#elif defined(FAPI_TEST_FINGERPRINT)
    size = asprintf(&config, "{\n"
                    "     \"profile_name\": \"%s\",\n"
                    "     \"profile_dir\": \"" TOP_SOURCEDIR "/test/data/fapi/\",\n"
                    "     \"user_dir\": \"%s/user/dir\",\n"
                    "     \"system_dir\": \"%s/system_dir\",\n"
                    "     \"system_pcrs\" : [],\n"
                    "     \"log_dir\" : \"%s\",\n"
                    "     \"tcti\": \"%s\",\n"
#if defined(FAPI_TEST_EK_CERT_LESS)
                    "     \"ek_cert_less\": \"yes\",\n"
#else
                    "     \"ek_fingerprint\": %s,\n"
#endif
                    "}\n",
                    profile, tmpdir, tmpdir, tmpdir,
                    getenv("TPM20TEST_TCTI")
#if !defined(FAPI_TEST_EK_CERT_LESS)
                    , getenv("FAPI_TEST_FINGERPRINT")
#endif
                   );
#elif defined(FAPI_TEST_CERTIFICATE)
    size = asprintf(&config, "{\n"
                    "     \"profile_name\": \"%s\",\n"
                    "     \"profile_dir\": \"" TOP_SOURCEDIR "/test/data/fapi/\",\n"
                    "     \"user_dir\": \"%s/user/dir\",\n"
                    "     \"system_dir\": \"%s/system_dir\",\n"
                    "     \"system_pcrs\" : [],\n"
                    "     \"log_dir\" : \"%s\",\n"
                    "     \"tcti\": \"%s\",\n"
#if defined(FAPI_TEST_EK_CERT_LESS)
                    "     \"ek_cert_less\": \"yes\",\n"
#else
                    "     \"ek_cert_file\": \"%s\",\n"
#endif
                    "}\n",
                    profile, tmpdir, tmpdir, tmpdir,
                    getenv("TPM20TEST_TCTI")
#if !defined(FAPI_TEST_EK_CERT_LESS)
                    , getenv("FAPI_TEST_CERTIFICATE")
#endif
                   );
#elif defined(FAPI_TEST_FINGERPRINT_ECC)
    size = asprintf(&config, "{\n"
                    "     \"profile_name\": \"%s\",\n"
                    "     \"profile_dir\": \"" TOP_SOURCEDIR "/test/data/fapi/\",\n"
                    "     \"user_dir\": \"%s/user/dir\",\n"
                    "     \"system_dir\": \"%s/system_dir\",\n"
                    "     \"system_pcrs\" : [],\n"
                    "     \"log_dir\" : \"%s\",\n"
                    "     \"tcti\": \"%s\",\n"
#if defined(FAPI_TEST_EK_CERT_LESS)
                    "     \"ek_cert_less\": \"yes\",\n"
#else
                    "     \"ek_fingerprint\": %s,\n"
#endif
                    "}\n",
                    profile, tmpdir, tmpdir, tmpdir,
                    getenv("TPM20TEST_TCTI")
#if !defined(FAPI_TEST_EK_CERT_LESS)
                    , getenv("FAPI_TEST_FINGERPRINT_ECC")
#endif
                   );
#elif defined(FAPI_TEST_CERTIFICATE_ECC)
    size = asprintf(&config, "{\n"
                    "     \"profile_name\": \"%s\",\n"
                    "     \"profile_dir\": \"" TOP_SOURCEDIR "/test/data/fapi/\",\n"
                    "     \"user_dir\": \"%s/user/dir\",\n"
                    "     \"system_dir\": \"%s/system_dir\",\n"
                    "     \"system_pcrs\" : [],\n"
                    "     \"log_dir\" : \"%s\",\n"
                    "     \"tcti\": \"%s\",\n"
#if defined(FAPI_TEST_EK_CERT_LESS)
                    "     \"ek_cert_less\": \"yes\",\n"
#else
                    "     \"ek_cert_file\": \"%s\",\n"
#endif
                    "}\n",
                    profile, tmpdir, tmpdir, tmpdir,
                    getenv("TPM20TEST_TCTI")
#if defined(FAPI_TEST_EK_CERT_LESS)
#else
                    , getenv("FAPI_TEST_CERTIFICATE_ECC")
#endif
                   );
#else /* FAPI_NONTPM */
    size = asprintf(&config, "{\n"
                    "     \"profile_name\": \"%s\",\n"
                    "     \"profile_dir\": \"" TOP_SOURCEDIR "/test/data/fapi/\",\n"
                    "     \"user_dir\": \"%s/user/dir\",\n"
                    "     \"system_dir\": \"%s/system_dir\",\n"
                    "     \"system_pcrs\" : [],\n"
                    "     \"log_dir\" : \"%s\",\n"
                    "     \"tcti\": \"%s\",\n"
#if defined(FAPI_TEST_EK_CERT_LESS)
                    "     \"ek_cert_less\": \"yes\",\n"
#endif
                    "",
                    profile, tmpdir, tmpdir, tmpdir,
                    getenv("TPM20TEST_TCTI"));
#endif /* FAPI_NONTPM */

    if (size < 0) {
        LOG_ERROR("Out of memory");
        ret = EXIT_ERROR;
        goto error;
    }

#if defined (FAPI_TEST_FIRMWARE_LOG_FILE)
    config_bak = config;
    size = asprintf(&config, "%s%s", config_bak, "     \"firmware_log_file\": \""  FAPI_TEST_FIRMWARE_LOG_FILE "\",\n");
    if (size < 0) {
        LOG_ERROR("Out of memory");
        ret = EXIT_ERROR;
        goto error;
    }
    SAFE_FREE(config_bak);
#endif
#if defined (FAPI_TEST_IMA_LOG_FILE)
    config_bak = config;
    size = asprintf(&config, "%s%s", config_bak, "     \"ima_log_file\": \"" FAPI_TEST_IMA_LOG_FILE "\",\n");
    if (size < 0) {
        LOG_ERROR("Out of memory");
        ret = EXIT_ERROR;
        goto error;
    }
    SAFE_FREE(config_bak);
#endif
#if defined (FAPI_TEST_FIRMWARE_LOG_FILE_ABS)
    config_bak = config;
    size = asprintf(&config, "%s%s", config_bak, "     \"firmware_log_file\": \"" FAPI_TEST_FIRMWARE_LOG_FILE_ABS "\",\n");
    if (size < 0) {
        LOG_ERROR("Out of memory");
        ret = EXIT_ERROR;
        goto error;
    }
    SAFE_FREE(config_bak);
#endif
#if defined (FAPI_TEST_IMA_LOG_FILE_ABS)
    config_bak = config;
    size = asprintf(&config, "%s%s", config_bak, "     \"ima_log_file\": \"" FAPI_TEST_IMA_LOG_FILE_ABS "\",\n");
    if (size < 0) {
        LOG_ERROR("Out of memory");
        ret = EXIT_ERROR;
        goto error;
    }
    SAFE_FREE(config_bak);
#endif


    config_bak = config;
    size = asprintf(&config, "%s}", config_bak);
    if (size < 0) {
        LOG_ERROR("Out of memory");
        ret = EXIT_ERROR;
        goto error;
    }
    SAFE_FREE(config_bak);

    if (size < 0) {
        LOG_ERROR("Out of memory");
        ret = EXIT_ERROR;
        goto error;
    }
    LOG_INFO("Using config:\n%s", config);

    /* We construct the path for the config file */
    size = asprintf(&config_path, "%s/fapi-config.json", tmpdir);
    if (size < 0) {
        LOG_ERROR("Out of memory");
        ret = EXIT_ERROR;
        goto error;
    }

    /* We write the config file to disk */
    config_file = fopen(config_path, "w");
    if (!config_file) {
        LOG_ERROR("Opening config file for writing");
        perror(config_path);
        ret = EXIT_ERROR;
        goto error;
    }
    size = fprintf(config_file, "%s", config);
    fclose(config_file);
    if (size < 0) {
        LOG_ERROR("Writing config file");
        perror(config_path);
        ret = EXIT_ERROR;
        goto error;
    }

    /* We set the environment variable for FAPI to consume the config file */
    size = asprintf(&config_env, "TSS2_FAPICONF=%s", config_path);
    if (size < 0) {
        LOG_ERROR("Out of memory");
        ret = EXIT_ERROR;
        goto error;
    }
    putenv(config_env);

    /***********
     * Call FAPI
     ***********/

    rc = Fapi_Initialize(fapi_context, NULL);
    if (rc != TSS2_RC_SUCCESS) {
        LOG_ERROR("Esys_Initialize FAILED! Response Code : 0x%x", rc);
        ret = EXIT_FAILURE;
        goto error;
    }
    fapi_test_ctx->fapi_ctx = *fapi_context;
    SAFE_FREE(config_env);
    SAFE_FREE(config);
    SAFE_FREE(config_path);
    return 0;

 error:
    Fapi_Finalize(fapi_context);

    if (config) free(config);
    if (config_path) free(config_path);
    if (config_env) free(config_env);

    return ret;
}

int
test_fapi_setup(TSS2_TEST_FAPI_CONTEXT **test_ctx)
{
    char template[] = "/tmp/fapi_tmpdir.XXXXXX";
    char *tmpdir = NULL;
    size_t size;
    int ret;

    size = sizeof(TSS2_TEST_FAPI_CONTEXT);
    *test_ctx = calloc(size, 1);
    if (test_ctx == NULL) {
        LOG_ERROR("Failed to allocate 0x%zx bytes for the test context", size);
        goto error;
    }

    tmpdir = strdup(template);
    if (!tmpdir) {
        LOG_ERROR("Failed to allocate name of temp dir.");
        goto error;
    }
    (*test_ctx)->tmpdir = mkdtemp(tmpdir);
    if (!(*test_ctx)->tmpdir) {
        LOG_ERROR("No temp dir created");
        goto error;
    }
    fapi_test_ctx = *test_ctx;

    ret = init_fapi(FAPI_PROFILE, &(*test_ctx)->fapi_ctx);
    if (ret != 0) {
        LOG_ERROR("init fapi failed.");
        goto error;
    }
    (*test_ctx)->test_esys_ctx.esys_ctx = (*test_ctx)->fapi_ctx->esys;
    (*test_ctx)->test_esys_ctx.tpm_state = malloc(sizeof(tpm_state));
    if (test_ctx == NULL) {
        LOG_ERROR("Failed to allocate 0x%zx bytes for tpm_state.", size);
        goto error;
    }

    return ret;

 error:
    SAFE_FREE(tmpdir);
    SAFE_FREE(*test_ctx);
    return EXIT_ERROR;
}

void
test_fapi_teardown(TSS2_TEST_FAPI_CONTEXT *test_ctx)
{
    if (test_ctx) {
        if (test_ctx->fapi_ctx) {
            Fapi_Finalize(&test_ctx->fapi_ctx);
        }
        SAFE_FREE(test_ctx->tmpdir);
        SAFE_FREE(test_ctx->test_esys_ctx.tpm_state);
        SAFE_FREE(test_ctx);
    }
}

/**
 * This program is a template for integration tests (ones that use the TCTI,
 * the ESAPI, and FAPI contexts / API directly). It does nothing more than
 * parsing  command line options that allow the caller (likely a script)
 * to specifywhich TCTI to use for the test using getenv("TPM20TEST_TCTI").
 */
int
main(int argc, char *argv[])
{
    int ret, size;
    char *config = NULL;
    char *config_path = NULL;
    char *config_env = NULL;
    char *remove_cmd = NULL;
    TSS2_TEST_FAPI_CONTEXT *test_ctx = NULL;

    ret = test_fapi_setup(&test_ctx);
    if (ret != 0) {
        goto error;
    }

#if !defined(FAPI_NONTPM) && !defined(DLOPEN)
    ret = test_fapi_checks_pre(test_ctx);
    if (ret != 0) {
        goto error;
    }
#endif

    ret = test_invoke_fapi(test_ctx->fapi_ctx);

    LOG_INFO("Test returned %i", ret);
    if (ret) goto error;

#if !defined(FAPI_NONTPM) && !defined(DLOPEN)
    test_ctx->test_esys_ctx.esys_ctx = test_ctx->fapi_ctx->esys;
    ret = test_fapi_checks_post(test_ctx);
    if (ret != 0) {
        goto error;
    }
#endif

    size = asprintf(&remove_cmd, "rm -r -f %s", test_ctx->tmpdir);
    if (size < 0) {
        LOG_ERROR("Out of memory");
        ret = EXIT_ERROR;
        goto error;
    }
    if (system(remove_cmd) != 0) {
        LOG_ERROR("Directory %s can't be deleted.", test_ctx->tmpdir);
        ret = EXIT_ERROR;
        goto error;
    }

error:
    test_fapi_teardown(test_ctx);
    if (config) free(config);
    if (config_path) free(config_path);
    if (config_env) free(config_env);
    if (remove_cmd) free(remove_cmd);

    return ret;
}
