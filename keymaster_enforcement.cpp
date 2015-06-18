/*
 * Copyright (C) 2014 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <keymaster/keymaster_enforcement.h>

#include <assert.h>
#include <string.h>

#include <limits>

#include <openssl/evp.h>

#include <hardware/hw_auth_token.h>
#include <keymaster/android_keymaster_utils.h>
#include <keymaster/logger.h>

using android::List;

namespace keymaster {

bool is_public_key_algorithm(const AuthorizationSet& auth_set) {
    keymaster_algorithm_t algorithm;
    return auth_set.GetTagValue(TAG_ALGORITHM, &algorithm) &&
           (algorithm == KM_ALGORITHM_RSA || algorithm == KM_ALGORITHM_EC);
}

static keymaster_error_t authorized_purpose(const keymaster_purpose_t purpose,
                                            const AuthorizationSet& auth_set) {
    switch (purpose) {
    case KM_PURPOSE_VERIFY:
    case KM_PURPOSE_ENCRYPT:
        if (is_public_key_algorithm(auth_set) || auth_set.Contains(TAG_PURPOSE, purpose))
            return KM_ERROR_OK;
        return KM_ERROR_INCOMPATIBLE_PURPOSE;

    case KM_PURPOSE_SIGN:
    case KM_PURPOSE_DECRYPT:
        if (auth_set.Contains(TAG_PURPOSE, purpose))
            return KM_ERROR_OK;
        return KM_ERROR_INCOMPATIBLE_PURPOSE;

    default:
        return KM_ERROR_UNSUPPORTED_PURPOSE;
    }
}

inline bool is_origination_purpose(keymaster_purpose_t purpose) {
    return purpose == KM_PURPOSE_ENCRYPT || purpose == KM_PURPOSE_SIGN;
}

inline bool is_usage_purpose(keymaster_purpose_t purpose) {
    return purpose == KM_PURPOSE_DECRYPT || purpose == KM_PURPOSE_VERIFY;
}

inline bool can_skip_authentication(bool is_begin_operation, bool is_auth_per_op_key) {
    // Durign begin with auth-per-op keys, we don't require authentication because it can't be
    // performed until after begin returns the operation handle used to for the authentication
    // challenge.
    return is_begin_operation && is_auth_per_op_key;
}

keymaster_error_t KeymasterEnforcement::AuthorizeOperation(const keymaster_purpose_t purpose,
                                                           const km_id_t keyid,
                                                           const AuthorizationSet& auth_set,
                                                           const AuthorizationSet& operation_params,
                                                           keymaster_operation_handle_t op_handle,
                                                           bool is_begin_operation) {
    // Find some entries that may be needed to handle KM_TAG_USER_SECURE_ID
    int auth_timeout_index = -1;
    int auth_type_index = -1;
    int no_auth_required_index = -1;
    for (size_t pos = 0; pos < auth_set.size(); ++pos) {
        switch (auth_set[pos].tag) {
        case KM_TAG_AUTH_TIMEOUT:
            auth_timeout_index = pos;
            break;
        case KM_TAG_USER_AUTH_TYPE:
            auth_type_index = pos;
            break;
        case KM_TAG_NO_AUTH_REQUIRED:
            no_auth_required_index = pos;
            break;
        default:
            break;
        }
    }

    keymaster_error_t error = authorized_purpose(purpose, auth_set);
    if (error != KM_ERROR_OK)
        return error;

    // If successful, and if key has a min time between ops, this will be set to the time limit
    uint32_t min_ops_timeout = UINT32_MAX;

    bool update_access_count = false;
    bool found_caller_nonce = false;
    bool authentication_required = false;
    bool auth_token_matched = false;

    for (auto& param : auth_set) {

        // KM_TAG_PADDING_OLD and KM_TAG_DIGEST_OLD aren't actually members of the enum, so we can't
        // switch on them.  There's nothing to validate for them, though, so just ignore them.
        if (param.tag == KM_TAG_PADDING_OLD || param.tag == KM_TAG_DIGEST_OLD)
            continue;

        switch (param.tag) {

        case KM_TAG_ACTIVE_DATETIME:
            if (!activation_date_valid(param.date_time))
                return KM_ERROR_KEY_NOT_YET_VALID;
            break;

        case KM_TAG_ORIGINATION_EXPIRE_DATETIME:
            if (is_origination_purpose(purpose) && expiration_date_passed(param.date_time))
                return KM_ERROR_KEY_EXPIRED;
            break;

        case KM_TAG_USAGE_EXPIRE_DATETIME:
            if (is_usage_purpose(purpose) && expiration_date_passed(param.date_time))
                return KM_ERROR_KEY_EXPIRED;
            break;

        case KM_TAG_MIN_SECONDS_BETWEEN_OPS:
            min_ops_timeout = param.integer;
            if (!MinTimeBetweenOpsPassed(min_ops_timeout, keyid))
                return KM_ERROR_KEY_RATE_LIMIT_EXCEEDED;
            break;

        case KM_TAG_MAX_USES_PER_BOOT:
            update_access_count = true;
            if (!MaxUsesPerBootNotExceeded(keyid, param.integer))
                return KM_ERROR_KEY_MAX_OPS_EXCEEDED;
            break;

        case KM_TAG_USER_SECURE_ID:
            if (no_auth_required_index != -1) {
                // Key has both KM_TAG_USER_SECURE_ID and KM_TAG_NO_AUTH_REQUIRED
                return KM_ERROR_INVALID_KEY_BLOB;
            } else if (!can_skip_authentication(is_begin_operation, auth_timeout_index == -1) ||
                       operation_params.find(KM_TAG_AUTH_TOKEN) != -1) {
                authentication_required = true;
                if (AuthTokenMatches(auth_set, operation_params, param.long_integer,
                                     auth_type_index, auth_timeout_index, op_handle,
                                     is_begin_operation))
                    auth_token_matched = true;
            }
            break;

        case KM_TAG_CALLER_NONCE:
            found_caller_nonce = true;
            break;

        /* Tags should never be in key auths. */
        case KM_TAG_INVALID:
        case KM_TAG_AUTH_TOKEN:
        case KM_TAG_ROOT_OF_TRUST:
        case KM_TAG_APPLICATION_DATA:
            return KM_ERROR_INVALID_KEY_BLOB;

        /* Tags used for cryptographic parameters. */
        case KM_TAG_PURPOSE:
        case KM_TAG_ALGORITHM:
        case KM_TAG_KEY_SIZE:
        case KM_TAG_BLOCK_MODE:
        case KM_TAG_DIGEST:
        case KM_TAG_MAC_LENGTH:
        case KM_TAG_PADDING:
        case KM_TAG_NONCE:

        /* Tags not used for operations. */
        case KM_TAG_BLOB_USAGE_REQUIREMENTS:

        /* Algorithm specific parameters not used for access control. */
        case KM_TAG_RSA_PUBLIC_EXPONENT:

        /* Informational tags. */
        case KM_TAG_CREATION_DATETIME:
        case KM_TAG_ORIGIN:
        case KM_TAG_ROLLBACK_RESISTANT:

        /* Tags handled when KM_TAG_USER_SECURE_ID is handled */
        case KM_TAG_NO_AUTH_REQUIRED:
        case KM_TAG_USER_AUTH_TYPE:
        case KM_TAG_AUTH_TIMEOUT:

        /* Tag to provide data to operations. */
        case KM_TAG_ASSOCIATED_DATA:

        /* Ignored pending removal */
        case KM_TAG_ALL_APPLICATIONS:
        case KM_TAG_APPLICATION_ID:
        case KM_TAG_USER_ID:
        case KM_TAG_ALL_USERS:
            break;

        case KM_TAG_BOOTLOADER_ONLY:
            return KM_ERROR_INVALID_KEY_BLOB;
        }
    }

    if (authentication_required && !auth_token_matched) {
        LOG_E("Auth required but no matching auth token found", 0);
        return KM_ERROR_KEY_USER_NOT_AUTHENTICATED;
    }

    if (!found_caller_nonce && operation_params.find(KM_TAG_NONCE) != -1)
        return KM_ERROR_CALLER_NONCE_PROHIBITED;

    if (min_ops_timeout != UINT32_MAX &&
        !access_time_map_.UpdateKeyAccessTime(keyid, get_current_time(), min_ops_timeout)) {
        LOG_E("Rate-limited keys table full.  Entries will time out.", 0);
        return KM_ERROR_TOO_MANY_OPERATIONS;
    }

    if (update_access_count && !access_count_map_.IncrementKeyAccessCount(keyid)) {
        LOG_E("Usage count-limited keys table full, until reboot.", 0);
        return KM_ERROR_TOO_MANY_OPERATIONS;
    }

    return KM_ERROR_OK;
}

class EvpMdCtx {
  public:
    EvpMdCtx() { EVP_MD_CTX_init(&ctx_); }
    ~EvpMdCtx() { EVP_MD_CTX_cleanup(&ctx_); }

    EVP_MD_CTX* get() { return &ctx_; }

  private:
    EVP_MD_CTX ctx_;
};

/* static */
bool KeymasterEnforcement::CreateKeyId(const keymaster_key_blob_t& key_blob, km_id_t* keyid) {
    EvpMdCtx ctx;

    uint8_t hash[EVP_MAX_MD_SIZE];
    unsigned int hash_len;
    if (EVP_DigestInit_ex(ctx.get(), EVP_sha256(), nullptr /* ENGINE */) &&
        EVP_DigestUpdate(ctx.get(), key_blob.key_material, key_blob.key_material_size) &&
        EVP_DigestFinal_ex(ctx.get(), hash, &hash_len)) {
        assert(hash_len >= sizeof(*keyid));
        memcpy(keyid, hash, sizeof(*keyid));
        return true;
    }

    return false;
}

bool KeymasterEnforcement::MinTimeBetweenOpsPassed(uint32_t min_time_between, const km_id_t keyid) {
    uint32_t last_access_time;
    if (!access_time_map_.LastKeyAccessTime(keyid, &last_access_time))
        return true;
    return min_time_between <= static_cast<int64_t>(get_current_time()) - last_access_time;
}

bool KeymasterEnforcement::MaxUsesPerBootNotExceeded(const km_id_t keyid, uint32_t max_uses) {
    uint32_t key_access_count;
    if (!access_count_map_.KeyAccessCount(keyid, &key_access_count))
        return true;
    return key_access_count < max_uses;
}

bool KeymasterEnforcement::AuthTokenMatches(const AuthorizationSet& auth_set,
                                            const AuthorizationSet& operation_params,
                                            const uint64_t user_secure_id,
                                            const int auth_type_index, const int auth_timeout_index,
                                            const keymaster_operation_handle_t op_handle,
                                            bool is_begin_operation) const {
    assert(auth_type_index < static_cast<int>(auth_set.size()));
    assert(auth_timeout_index < static_cast<int>(auth_set.size()));

    keymaster_blob_t auth_token_blob;
    if (!operation_params.GetTagValue(TAG_AUTH_TOKEN, &auth_token_blob)) {
        LOG_E("Authentication required, but auth token not provided", 0);
        return false;
    }

    if (auth_token_blob.data_length != sizeof(hw_auth_token_t)) {
        LOG_E("Bug: Auth token is the wrong size (%d expected, %d found)", sizeof(hw_auth_token_t),
              auth_token_blob.data_length);
        return false;
    }

    hw_auth_token_t auth_token;
    memcpy(&auth_token, auth_token_blob.data, sizeof(hw_auth_token_t));
    if (auth_token.version != HW_AUTH_TOKEN_VERSION) {
        LOG_E("Bug: Auth token is the version %d (or is not an auth token). Expected %d",
              auth_token.version, HW_AUTH_TOKEN_VERSION);
        return false;
    }

    if (!ValidateTokenSignature(auth_token)) {
        LOG_E("Auth token signature invalid", 0);
        return false;
    }

    if (auth_timeout_index == -1 && op_handle && op_handle != auth_token.challenge) {
        LOG_E("Auth token has the challenge %llu, need %llu", auth_token.challenge, op_handle);
        return false;
    }

    if (user_secure_id != auth_token.user_id && user_secure_id != auth_token.authenticator_id) {
        LOG_I("Auth token SIDs %llu and %llu do not match key SID %llu", auth_token.user_id,
              auth_token.authenticator_id, user_secure_id);
        return false;
    }

    if (auth_type_index < 0 || auth_type_index > static_cast<int>(auth_set.size())) {
        LOG_E("Auth required but no auth type found", 0);
        return false;
    }

    assert(auth_set[auth_type_index].tag == KM_TAG_USER_AUTH_TYPE);
    if (auth_set[auth_type_index].tag != KM_TAG_USER_AUTH_TYPE)
        return false;

    uint32_t key_auth_type_mask = auth_set[auth_type_index].integer;
    uint32_t token_auth_type = ntoh(auth_token.authenticator_type);
    if ((key_auth_type_mask & token_auth_type) == 0) {
        LOG_E("Key requires match of auth type mask 0%uo, but token contained 0%uo",
              key_auth_type_mask, token_auth_type);
        return false;
    }

    if (auth_timeout_index != -1 && is_begin_operation) {
        assert(auth_set[auth_timeout_index].tag == KM_TAG_AUTH_TIMEOUT);
        if (auth_set[auth_timeout_index].tag != KM_TAG_AUTH_TIMEOUT)
            return false;

        if (auth_token_timed_out(auth_token, auth_set[auth_timeout_index].integer)) {
            LOG_E("Auth token has timed out", 0);
            return false;
        }
    }

    // Survived the whole gauntlet.  We have authentage!
    return true;
}

bool KeymasterEnforcement::AccessTimeMap::LastKeyAccessTime(km_id_t keyid,
                                                            uint32_t* last_access_time) const {
    for (auto& entry : last_access_list_)
        if (entry.keyid == keyid) {
            *last_access_time = entry.access_time;
            return true;
        }
    return false;
}

bool KeymasterEnforcement::AccessTimeMap::UpdateKeyAccessTime(km_id_t keyid, uint32_t current_time,
                                                              uint32_t timeout) {
    List<AccessTime>::iterator iter;
    for (iter = last_access_list_.begin(); iter != last_access_list_.end();) {
        if (iter->keyid == keyid) {
            iter->access_time = current_time;
            return true;
        }

        // Expire entry if possible.
        assert(current_time >= iter->access_time);
        if (current_time - iter->access_time >= iter->timeout)
            iter = last_access_list_.erase(iter);
        else
            ++iter;
    }

    if (last_access_list_.size() >= max_size_)
        return false;

    AccessTime new_entry;
    new_entry.keyid = keyid;
    new_entry.access_time = current_time;
    new_entry.timeout = timeout;
    last_access_list_.push_front(new_entry);
    return true;
}

bool KeymasterEnforcement::AccessCountMap::KeyAccessCount(km_id_t keyid, uint32_t* count) const {
    for (auto& entry : access_count_list_)
        if (entry.keyid == keyid) {
            *count = entry.access_count;
            return true;
        }
    return false;
}

template <typename T> T max_value(T) {
    return std::numeric_limits<T>::max();
}

bool KeymasterEnforcement::AccessCountMap::IncrementKeyAccessCount(km_id_t keyid) {
    for (auto& entry : access_count_list_)
        if (entry.keyid == keyid) {
            if (entry.access_count < max_value(entry.access_count))
                ++entry.access_count;
            return true;
        }

    if (access_count_list_.size() >= max_size_)
        return false;

    AccessCount new_entry;
    new_entry.keyid = keyid;
    new_entry.access_count = 1;
    access_count_list_.push_front(new_entry);
    return true;
}
}; /* namespace keymaster */
