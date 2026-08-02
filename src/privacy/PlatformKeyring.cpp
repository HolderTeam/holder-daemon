#include "privacy/PlatformKeyring.h"

#include "privacy/SecretStore.h"

#if HOLDER_HAVE_LIBSECRET
#include <libsecret/secret.h>
#endif

#if HOLDER_HAVE_MACOS_KEYCHAIN
#include <CoreFoundation/CoreFoundation.h>
#include <Security/Security.h>
#endif

#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace holder::privacy {
namespace {

constexpr const char* kProjectKeyService = "org.holder.ProjectKey";

PlatformKeyringLookupHook& lookup_hook_storage() {
  static PlatformKeyringLookupHook hook = nullptr;
  return hook;
}

PlatformKeyringStoreHook& store_hook_storage() {
  static PlatformKeyringStoreHook hook = nullptr;
  return hook;
}

PlatformKeyringRemoveHook& remove_hook_storage() {
  static PlatformKeyringRemoveHook hook = nullptr;
  return hook;
}

std::string missing_project_id_message() {
  return "project key lookup missing project id";
} // LCOV_EXCL_LINE

#if HOLDER_HAVE_LIBSECRET
// LCOV_EXCL_START: direct libsecret schema/backend calls depend on host keyring services.
const SecretSchema* holder_secret_schema() {
  static const SecretSchema schema = {
      "org.holder.SecretStore",
      SECRET_SCHEMA_NONE,
      {
          {"service", SECRET_SCHEMA_ATTRIBUTE_STRING},
          {"account", SECRET_SCHEMA_ATTRIBUTE_STRING},
          {nullptr, static_cast<SecretSchemaAttributeType>(0)},
      },
      0,
      nullptr,
      nullptr,
      nullptr,
      nullptr,
      nullptr,
      nullptr,
      nullptr
  };
  return &schema;
}

const SecretSchema* holder_project_key_schema() {
  static const SecretSchema schema = {
      kProjectKeyService,
      SECRET_SCHEMA_NONE,
      {
          {"key_id", SECRET_SCHEMA_ATTRIBUTE_STRING},
          {"project_id", SECRET_SCHEMA_ATTRIBUTE_STRING},
          {nullptr, static_cast<SecretSchemaAttributeType>(0)},
      },
      0,
      nullptr,
      nullptr,
      nullptr,
      nullptr,
      nullptr,
      nullptr,
      nullptr
  };
  return &schema;
}
// LCOV_EXCL_STOP

LibsecretApiLookupHook& libsecret_api_lookup_hook_storage() {
  static LibsecretApiLookupHook hook = nullptr;
  return hook;
}

LibsecretLookupHook& libsecret_lookup_hook_storage() {
  static LibsecretLookupHook hook = nullptr;
  return hook;
}

PlatformKeyringLookupResult libsecret_lookup_generic(
    const std::string& service,
    const std::string& account
) {
  if (const auto hook = libsecret_api_lookup_hook_storage(); hook != nullptr) {
    const auto result = hook(service, account);
    return {.secret = result.secret, .error_message = result.error_message};
  }
  // LCOV_EXCL_START: exercised via low-level seam tests; real libsecret calls depend on host
  // keyring state.
  GError* error = nullptr;
  gchar* secret = secret_password_lookup_sync(
      holder_secret_schema(),
      nullptr,
      &error,
      "service",
      service.c_str(),
      "account",
      account.c_str(),
      nullptr
  );
  if (!secret) {
    std::optional<std::string> message;
    if (error) {
      message = "secret lookup failed in libsecret";
      if (error->message) {
        *message += ": ";
        *message += error->message;
      }
      g_error_free(error);
    }
    return {.secret = std::nullopt, .error_message = std::move(message)};
  }
  std::string out(secret);
  secret_password_free(secret);
  return {.secret = std::move(out), .error_message = std::nullopt};
  // LCOV_EXCL_STOP
}

// LCOV_EXCL_START: direct libsecret lookup depends on host keyring services.
PlatformKeyringLookupResult libsecret_lookup_project_key(
    const std::string& project_id,
    const std::string& key_id
) {
  GError* error = nullptr;
  gchar* secret = secret_password_lookup_sync(
      holder_project_key_schema(),
      nullptr,
      &error,
      "key_id",
      key_id.c_str(),
      "project_id",
      project_id.c_str(),
      nullptr
  );
  if (!secret) {
    std::string message = "project key material not found in libsecret";
    if (error && error->message) {
      message += ": ";
      message += error->message;
    }
    if (error) {
      g_error_free(error);
    }
    return {.secret = std::nullopt, .error_message = std::move(message)};
  }
  std::string out(secret);
  secret_password_free(secret);
  return {.secret = std::move(out), .error_message = std::nullopt};
}
// LCOV_EXCL_STOP

PlatformKeyringLookupResult libsecret_lookup_secret(const PlatformKeyringSecretRef& ref) {
  if (ref.kind == PlatformKeyringSecretKind::GenericSecret) {
    if (const auto hook = libsecret_lookup_hook_storage()) {
      const auto result = hook(ref.service, ref.account);
      return {.secret = result.secret, .error_message = result.error_message};
    }
    return libsecret_lookup_generic(ref.service, ref.account);
  }

  if (!ref.project_id.has_value()) {
    return {.secret = std::nullopt, .error_message = missing_project_id_message()};
  }
  return libsecret_lookup_project_key(ref.project_id.value(), ref.account); // LCOV_EXCL_LINE
}

// LCOV_EXCL_START: direct libsecret store/remove depends on host keyring services.
void libsecret_store_secret(
    const PlatformKeyringSecretRef& ref,
    const std::string& label,
    const std::string& secret
) {
  GError* error = nullptr;
  bool ok = false;
  if (ref.kind == PlatformKeyringSecretKind::GenericSecret) {
    ok = secret_password_store_sync(
        holder_secret_schema(),
        SECRET_COLLECTION_DEFAULT,
        label.c_str(),
        secret.c_str(),
        nullptr,
        &error,
        "service",
        ref.service.c_str(),
        "account",
        ref.account.c_str(),
        nullptr
    );
  } else if (ref.project_id.has_value()) {
    ok = secret_password_store_sync(
        holder_project_key_schema(),
        SECRET_COLLECTION_DEFAULT,
        label.c_str(),
        secret.c_str(),
        nullptr,
        &error,
        "key_id",
        ref.account.c_str(),
        "project_id",
        ref.project_id->c_str(),
        nullptr
    );
  } else {
    throw PrivacyError(PrivacyErrorCode::KeyringUnavailable, missing_project_id_message());
  }

  if (!ok) {
    std::string message = "failed to store secret in libsecret";
    if (error && error->message) {
      message += ": ";
      message += error->message;
    }
    if (error) {
      g_error_free(error);
    }
    throw PrivacyError(PrivacyErrorCode::KeyringUnavailable, message);
  }
}

void libsecret_remove_secret(const PlatformKeyringSecretRef& ref) {
  GError* error = nullptr;
  bool ok = false;
  if (ref.kind == PlatformKeyringSecretKind::GenericSecret) {
    ok = secret_password_clear_sync(
        holder_secret_schema(),
        nullptr,
        &error,
        "service",
        ref.service.c_str(),
        "account",
        ref.account.c_str(),
        nullptr
    );
  } else if (ref.project_id.has_value()) {
    ok = secret_password_clear_sync(
        holder_project_key_schema(),
        nullptr,
        &error,
        "key_id",
        ref.account.c_str(),
        "project_id",
        ref.project_id->c_str(),
        nullptr
    );
  } else {
    throw PrivacyError(PrivacyErrorCode::KeyringUnavailable, missing_project_id_message());
  }

  if (!ok && error) {
    std::string message = "failed to remove secret from libsecret";
    if (error->message) {
      message += ": ";
      message += error->message;
    }
    g_error_free(error);
    throw PrivacyError(PrivacyErrorCode::KeyringUnavailable, message);
  }
}
// LCOV_EXCL_STOP
#endif

#if HOLDER_HAVE_MACOS_KEYCHAIN
// LCOV_EXCL_START: direct Keychain calls depend on the host login keychain.
template <typename T>
class CfRef {
 public:
  explicit CfRef(T value = nullptr)
      : value_(value) {}
  ~CfRef() {
    if (value_ != nullptr) {
      CFRelease(value_);
    }
  }
  CfRef(const CfRef&) = delete;
  CfRef& operator=(const CfRef&) = delete;
  CfRef(CfRef&& other) noexcept
      : value_(other.value_) {
    other.value_ = nullptr;
  }
  CfRef& operator=(CfRef&& other) noexcept {
    if (this != &other) {
      if (value_ != nullptr) {
        CFRelease(value_);
      }
      value_ = other.value_;
      other.value_ = nullptr;
    }
    return *this;
  }
  T get() const { return value_; }

 private:
  T value_;
};

CFIndex checked_cf_index(std::size_t size) {
  const auto max_cf_index = static_cast<std::size_t>(std::numeric_limits<CFIndex>::max());
  if (size > max_cf_index) {
    throw PrivacyError(PrivacyErrorCode::KeyringUnavailable, "secret too large for Keychain");
  }
  return static_cast<CFIndex>(size);
}

CfRef<CFStringRef> cf_string(const std::string& value) {
  return CfRef<CFStringRef>(CFStringCreateWithBytes(
      nullptr,
      reinterpret_cast<const UInt8*>(value.data()),
      checked_cf_index(value.size()),
      kCFStringEncodingUTF8,
      false
  ));
}

CfRef<CFDataRef> cf_data(const std::string& value) {
  return CfRef<CFDataRef>(CFDataCreate(
      nullptr,
      reinterpret_cast<const UInt8*>(value.data()),
      checked_cf_index(value.size())
  ));
}

std::string string_from_cf_string(CFStringRef value) {
  if (value == nullptr) {
    return {};
  }
  const CFIndex length = CFStringGetLength(value);
  if (length <= 0) {
    return {};
  }
  const CFIndex max_size = CFStringGetMaximumSizeForEncoding(length, kCFStringEncodingUTF8) + 1;
  if (max_size <= 0) {
    return {};
  }
  std::vector<char> buffer(static_cast<std::size_t>(max_size));
  if (!CFStringGetCString(value, buffer.data(), max_size, kCFStringEncodingUTF8)) {
    return {};
  }
  return std::string(buffer.data());
}

std::string keychain_status_message(OSStatus status, const std::string& action) {
  std::string message = action + " in macOS Keychain";
  const CfRef<CFStringRef> detail(SecCopyErrorMessageString(status, nullptr));
  const auto detail_text = string_from_cf_string(detail.get());
  if (!detail_text.empty()) {
    message += ": ";
    message += detail_text;
  } else {
    message += ": OSStatus ";
    message += std::to_string(status);
  }
  return message;
}

std::string keychain_service(const PlatformKeyringSecretRef& ref) {
  if (ref.kind == PlatformKeyringSecretKind::ProjectKey) {
    if (!ref.project_id.has_value()) {
      throw PrivacyError(PrivacyErrorCode::KeyringUnavailable, missing_project_id_message());
    }
    return std::string(kProjectKeyService) + "/" + ref.project_id.value();
  }
  return ref.service;
}

CfRef<CFMutableDictionaryRef> make_keychain_query(const PlatformKeyringSecretRef& ref) {
  auto query = CfRef<CFMutableDictionaryRef>(CFDictionaryCreateMutable(
      nullptr,
      0,
      &kCFTypeDictionaryKeyCallBacks,
      &kCFTypeDictionaryValueCallBacks
  ));
  if (query.get() == nullptr) {
    throw PrivacyError(
        PrivacyErrorCode::KeyringUnavailable,
        "failed to allocate macOS Keychain query"
    );
  }

  const auto service = cf_string(keychain_service(ref));
  const auto account = cf_string(ref.account);
  if (service.get() == nullptr || account.get() == nullptr) {
    throw PrivacyError(
        PrivacyErrorCode::KeyringUnavailable,
        "failed to allocate macOS Keychain attributes"
    );
  }

  CFDictionarySetValue(query.get(), kSecClass, kSecClassGenericPassword);
  CFDictionarySetValue(query.get(), kSecAttrService, service.get());
  CFDictionarySetValue(query.get(), kSecAttrAccount, account.get());
  return query;
}

std::string string_from_cf_data(CFDataRef value) {
  const CFIndex length = CFDataGetLength(value);
  if (length <= 0) {
    return {};
  }
  const UInt8* bytes = CFDataGetBytePtr(value);
  if (bytes == nullptr) {
    return {};
  }
  return std::string(reinterpret_cast<const char*>(bytes), static_cast<std::size_t>(length));
}

PlatformKeyringLookupResult keychain_lookup_secret(const PlatformKeyringSecretRef& ref) {
  auto query = make_keychain_query(ref);
  CFDictionarySetValue(query.get(), kSecReturnData, kCFBooleanTrue);
  CFDictionarySetValue(query.get(), kSecMatchLimit, kSecMatchLimitOne);

  CFTypeRef raw_result = nullptr;
  const OSStatus status = SecItemCopyMatching(query.get(), &raw_result);
  if (status == errSecItemNotFound) {
    return {.secret = std::nullopt, .error_message = std::nullopt};
  }
  if (status != errSecSuccess) {
    return {
        .secret = std::nullopt,
        .error_message = keychain_status_message(status, "secret lookup failed")
    };
  }

  CfRef<CFTypeRef> result(raw_result);
  if (CFGetTypeID(result.get()) != CFDataGetTypeID()) {
    return {
        .secret = std::nullopt,
        .error_message = std::string("secret lookup failed in macOS Keychain: result was not data")
    };
  }
  return {
      .secret = string_from_cf_data(static_cast<CFDataRef>(result.get())),
      .error_message = std::nullopt
  };
}

void keychain_store_secret(
    const PlatformKeyringSecretRef& ref,
    const std::string& label,
    const std::string& secret
) {
  auto query = make_keychain_query(ref);
  const auto secret_data = cf_data(secret);
  const auto label_text = cf_string(label);
  if (secret_data.get() == nullptr || label_text.get() == nullptr) {
    throw PrivacyError(
        PrivacyErrorCode::KeyringUnavailable,
        "failed to allocate macOS Keychain value"
    );
  }

  auto update_attrs = CfRef<CFMutableDictionaryRef>(CFDictionaryCreateMutable(
      nullptr,
      0,
      &kCFTypeDictionaryKeyCallBacks,
      &kCFTypeDictionaryValueCallBacks
  ));
  if (update_attrs.get() == nullptr) {
    throw PrivacyError(
        PrivacyErrorCode::KeyringUnavailable,
        "failed to allocate macOS Keychain update"
    );
  }
  CFDictionarySetValue(update_attrs.get(), kSecValueData, secret_data.get());
  CFDictionarySetValue(update_attrs.get(), kSecAttrLabel, label_text.get());

  const OSStatus update_status = SecItemUpdate(query.get(), update_attrs.get());
  if (update_status == errSecSuccess) {
    return;
  }
  if (update_status != errSecItemNotFound) {
    throw PrivacyError(
        PrivacyErrorCode::KeyringUnavailable,
        keychain_status_message(update_status, "failed to update secret")
    );
  }

  auto add_query = make_keychain_query(ref);
  CFDictionarySetValue(add_query.get(), kSecValueData, secret_data.get());
  CFDictionarySetValue(add_query.get(), kSecAttrLabel, label_text.get());
  const OSStatus add_status = SecItemAdd(add_query.get(), nullptr);
  if (add_status != errSecSuccess) {
    throw PrivacyError(
        PrivacyErrorCode::KeyringUnavailable,
        keychain_status_message(add_status, "failed to store secret")
    );
  }
}

void keychain_remove_secret(const PlatformKeyringSecretRef& ref) {
  auto query = make_keychain_query(ref);
  const OSStatus status = SecItemDelete(query.get());
  if (status != errSecSuccess && status != errSecItemNotFound) {
    throw PrivacyError(
        PrivacyErrorCode::KeyringUnavailable,
        keychain_status_message(status, "failed to remove secret")
    );
  }
}
// LCOV_EXCL_STOP
#endif

bool compiled_platform_keyring_supported() {
#if HOLDER_HAVE_LIBSECRET || HOLDER_HAVE_MACOS_KEYCHAIN
  return true;
#else
  return false;
#endif
}

} // namespace

bool platform_keyring_supported() { return compiled_platform_keyring_supported(); }

PlatformKeyringLookupResult platform_keyring_lookup_secret(const PlatformKeyringSecretRef& ref) {
  if (const auto hook = lookup_hook_storage()) {
    return hook(ref);
  }
#if HOLDER_HAVE_LIBSECRET
  return libsecret_lookup_secret(ref);
#elif HOLDER_HAVE_MACOS_KEYCHAIN
  return keychain_lookup_secret(ref);
#else
  return {
      .secret = std::nullopt,
      .error_message = std::string("platform keyring support not available")
  };
#endif
}

void platform_keyring_store_secret(
    const PlatformKeyringSecretRef& ref,
    const std::string& label,
    const std::string& secret
) {
  if (const auto hook = store_hook_storage()) {
    if (const auto error = hook(ref, secret); error.has_value()) {
      throw PrivacyError(PrivacyErrorCode::KeyringUnavailable, *error);
    }
    return;
  }
#if HOLDER_HAVE_LIBSECRET
  libsecret_store_secret(ref, label, secret); // LCOV_EXCL_LINE
#elif HOLDER_HAVE_MACOS_KEYCHAIN
  keychain_store_secret(ref, label, secret);
#else
  (void)ref;
  (void)label;
  (void)secret;
  throw PrivacyError(
      PrivacyErrorCode::KeyringUnavailable,
      "platform keyring support not available and HOLDER_TEST_KEYSTORE_DIR not set"
  );
#endif
}

void platform_keyring_remove_secret(const PlatformKeyringSecretRef& ref) {
  if (const auto hook = remove_hook_storage()) {
    if (const auto error = hook(ref); error.has_value()) {
      throw PrivacyError(PrivacyErrorCode::KeyringUnavailable, *error);
    }
    return;
  }
#if HOLDER_HAVE_LIBSECRET
  libsecret_remove_secret(ref); // LCOV_EXCL_LINE
#elif HOLDER_HAVE_MACOS_KEYCHAIN
  keychain_remove_secret(ref);
#else
  (void)ref;
  throw PrivacyError(
      PrivacyErrorCode::KeyringUnavailable,
      "platform keyring support not available and HOLDER_TEST_KEYSTORE_DIR not set"
  );
#endif
}

void platform_keyring_set_lookup_hook_for_tests(PlatformKeyringLookupHook hook) {
  lookup_hook_storage() = hook;
}

void platform_keyring_set_store_hook_for_tests(PlatformKeyringStoreHook hook) {
  store_hook_storage() = hook;
}

void platform_keyring_set_remove_hook_for_tests(PlatformKeyringRemoveHook hook) {
  remove_hook_storage() = hook;
}

#if HOLDER_HAVE_LIBSECRET
void secret_store_set_libsecret_lookup_hook_for_tests(LibsecretLookupHook hook) {
  libsecret_lookup_hook_storage() = hook;
}

void secret_store_set_libsecret_api_lookup_hook_for_tests(LibsecretApiLookupHook hook) {
  libsecret_api_lookup_hook_storage() = hook;
}
#endif

} // namespace holder::privacy
