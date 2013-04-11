/***
  This file is part of fusedav.

  fusedav is free software; you can redistribute it and/or modify it
  under the terms of the GNU General Public License as published by
  the Free Software Foundation; either version 2 of the License, or
  (at your option) any later version.

  fusedav is distributed in the hope that it will be useful, but WITHOUT
  ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
  or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public
  License for more details.

  You should have received a copy of the GNU General Public License
  along with fusedav; if not, write to the Free Software Foundation,
  Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA

  This file also contains code derived from W3C test code:
  http://dev.w3.org/XML/testDAV.c
***/

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stdlib.h>
#include <expat.h>
#include <curl/curl.h>
#include <time.h>
#include <unistd.h> // For getuid/getgid.

#include "log.h"
#include "props.h"
#include "session.h"
#include "util.h"

struct response_state {
    char path[PATH_MAX];
    unsigned long status_code;
    struct stat st;
};

struct element_state {
    char *current_data;
    size_t current_data_len;
};

struct propfind_state {
    props_result_callback callback;
    void *userdata;
    struct response_state rstate;
    struct element_state estate;
};

static void startElement(__unused void *userData, __unused const XML_Char *name, __unused const XML_Char **atts) {
    struct propfind_state *state = (struct propfind_state *) userData;

    state->estate.current_data = malloc(1);
    state->estate.current_data[0] = '\0';
    state->estate.current_data_len = 1;
}

static void characterDataHandler(void *userData, const XML_Char *s, int len) {
    struct propfind_state *state = (struct propfind_state *) userData;

    // Extend the size of current_data by len. If it's uninitialized,
    // realloc will malloc for size len.
    state->estate.current_data = realloc(state->estate.current_data, state->estate.current_data_len + len);

    // Copy in the new data, starting by overwriting the NUL terminator.
    strncpy(state->estate.current_data + state->estate.current_data_len - 1, s, len);

    // Update the string length.
    state->estate.current_data_len += len;

    // NUL-terminate the new string.
    state->estate.current_data[state->estate.current_data_len - 1] = '\0';
}

static void endElement(void *userData, const XML_Char *name) {
    struct propfind_state *state = (struct propfind_state *) userData;

    if (strcmp(name, "status") == 0) {
        char *token_status;
        strtok_r(state->estate.current_data, " ", &token_status);
        state->rstate.status_code = (unsigned long) atol(strtok_r(NULL, " ", &token_status));
    }
    else if (strcmp(name, "href") == 0) {
        log_print(LOG_INFO, "href: %s", state->estate.current_data);
        if (strstr(state->estate.current_data, get_base_host()) == state->estate.current_data) {
            size_t path_len;
            strncpy(state->rstate.path, state->estate.current_data + strlen(get_base_host()), PATH_MAX);
            // Trim trailing slash, if any.
            path_len = strlen(state->rstate.path);
            if (state->rstate.path[path_len - 1] == '/')
                state->rstate.path[path_len - 1] = '\0';
        }
    }
    // @TODO: Update Valhalla server to use HTTP/1.1 410 Gone instead.
    else if (strcmp(name, "event") == 0) {
        if (strcmp(state->estate.current_data, "DESTROYED") == 0) {
            state->rstate.status_code = 410;
        }
    }
    else if (strcmp(name, "resourcetype") == 0) {
        if (state->estate.current_data != NULL && strstr(state->estate.current_data, "collection") != NULL)
            state->rstate.st.st_mode |= S_IFDIR;
    }
    else if (strcmp(name, "getcontentlength") == 0) {
        state->rstate.st.st_size = atol(state->estate.current_data);
    }
    else if (strcmp(name, "getlastmodified") == 0) {
        state->rstate.st.st_mtime = curl_getdate(state->estate.current_data, NULL);
        state->rstate.st.st_atime = state->rstate.st.st_mtime;
    }
    else if (strcmp(name, "creationdate") == 0) {
        struct tm t;
        strptime(state->estate.current_data, "%FT%H:%M:%S%z", &t);
        state->rstate.st.st_ctime = mktime(&t);
    }
    else if (strcmp(name, "response") == 0) {
        // Default to a normal file if it's not explicitly a directory.
        if (state->rstate.st.st_mode & S_IFDIR) {
            state->rstate.st.st_mode |= 0770;
            state->rstate.st.st_nlink = 3;
        }
        else {
            state->rstate.st.st_mode |= S_IFREG | 0660;
            state->rstate.st.st_nlink = 1;
        }

        state->rstate.st.st_uid = getuid();
        state->rstate.st.st_gid = getgid();

        //log_print(LOG_DEBUG, "Path: %s", state->rstate.path);
        //log_print(LOG_DEBUG, "    Status code: %lu", state->rstate.status_code);
        //log_print(LOG_DEBUG, "    Size: %u", state->rstate.st.st_size);
        //log_print(LOG_DEBUG, "    Modified: %u", state->rstate.st.st_mtime);
        //log_print(LOG_DEBUG, "    Created: %u", state->rstate.st.st_ctime);

        // Invoke the callback.
        state->callback(state->userdata, state->rstate.path, state->rstate.st, state->rstate.status_code);

        // Reset response state.
        memset(&state->rstate, 0, sizeof(struct response_state));
    }

    // Reset element state.
    free(state->estate.current_data);
    memset(&state->estate, 0, sizeof(struct element_state));
}

static size_t write_parsing_callback(void *contents, size_t length, size_t nmemb, void *userp) {
    XML_Parser parser = (XML_Parser) userp;
    size_t real_size = length * nmemb;

    log_print(LOG_DEBUG, "Got chunk of %u bytes.", real_size);

    if (XML_Parse(parser, contents, real_size, 0) == 0) {
        int error_code = XML_GetErrorCode(parser);
        log_print(LOG_WARNING, "Parsing response buffer of length %u failed with error: %s", real_size, XML_ErrorString(error_code));
        return 0; // Zero bytes processed is failure.
    }

    return real_size;
}

int simple_propfind(const char *path, size_t depth, props_result_callback results, void *userdata) {
    // Local variables for cURL.
    CURL *session = session_request_init(path);
    struct curl_slist *slist = NULL;
    CURLcode res;
    char *header = NULL;
    unsigned long response_code;

    // Local variables for Expat and parsing.
    XML_Parser parser;
    struct propfind_state state;

    int ret = -1;

    // Set a blank initial state, except for the callback.
    memset(&state, 0, sizeof(struct propfind_state));
    state.callback = results;
    state.userdata = userdata;

    // Configure the parser.
    parser = XML_ParserCreate(NULL);
    XML_SetUserData(parser, &state);
    XML_SetElementHandler(parser, startElement, endElement);
    XML_SetCharacterDataHandler(parser, characterDataHandler);
    curl_easy_setopt(session, CURLOPT_WRITEDATA, (void *) parser);
    curl_easy_setopt(session, CURLOPT_WRITEFUNCTION, write_parsing_callback);

    // Add the Depth header and PROPFIND verb.
    curl_easy_setopt(session, CURLOPT_CUSTOMREQUEST, "PROPFIND");
    asprintf(&header, "Depth: %lu", depth);
    slist = curl_slist_append(slist, header);
    free(header);
    curl_easy_setopt(session, CURLOPT_HTTPHEADER, slist);

    /* @TODO: Send the proper PROPFIND body:
     * <?xml version="1.0" encoding="utf-8" ?>
     * <D:propfind xmlns:D="DAV:"><D:allprop/></D:propfind>
     */

    // Perform the request and parse the response.
    log_print(LOG_INFO, "About to perform PROPFIND.");
    res = curl_easy_perform(session);

    // Finalize parsing.
    if (XML_Parse(parser, NULL, 0, 1) == 0) {
        int error_code = XML_GetErrorCode(parser);
        log_print(LOG_WARNING, "Finalizing parsing failed with error: %s", XML_ErrorString(error_code));
    }

    if (res != CURLE_OK) {
        log_print(LOG_WARNING, "PROPFIND failed: %s", curl_easy_strerror(res));
        goto finish;
    }

    curl_easy_getinfo(session, CURLINFO_RESPONSE_CODE, &response_code);
    if (response_code != 207) {
        log_print(LOG_WARNING, "PROPFIND failed with response code: %u", response_code);
        goto finish;
    }

    ret = 0;

finish:
    curl_slist_free_all(slist);
    XML_ParserFree(parser);
    return ret;
}
