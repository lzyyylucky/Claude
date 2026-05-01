#pragma once

#include "storage_iface.hpp"

extern "C" {
void* cct_sql_bundle_open(const char* conn_utf8, char* err, size_t err_cap);
void cct_sql_bundle_close(void* bundle);
cct::storage::IUserPersistence* cct_sql_bundle_users(void* bundle);
cct::storage::IChatPersistence* cct_sql_bundle_chats(void* bundle);
cct::storage::IComponentPersistence* cct_sql_bundle_components(void* bundle);
cct::storage::ITokenBillingPersistence* cct_sql_bundle_token_billing(void* bundle);
}
