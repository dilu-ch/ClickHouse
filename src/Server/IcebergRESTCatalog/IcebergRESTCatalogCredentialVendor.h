#pragma once

#include <base/types.h>
#include <Server/IcebergRESTCatalog/IcebergRESTCatalogWarehouseDefinition.h>

#include <map>
#include <memory>
#include <mutex>

namespace DB::S3
{
class Client;
class AWSAssumeRoleClient;
}

namespace DB
{

enum class IcebergRESTCatalogVendedAccess : uint8_t
{
    ReadOnly,
    ReadWrite,
};

/// Temporary S3 credentials for one table, in the shape of the Iceberg REST `config` map.
struct IcebergRESTCatalogVendedCredentials
{
    /// The table location. It is the `prefix` of a `StorageCredential`.
    String prefix;
    std::map<String, String> config;
    Int64 expires_at_ms = 0;
};

/// Issues S3 credentials for one table of a warehouse (RFC: issue #114697).
/// The server assumes the warehouse role with a session policy. The policy limits the role to the table location.
class IcebergRESTCatalogCredentialVendor
{
public:
    /// `sts_client_` signs with the identity of the server.
    /// `s3_endpoint_` is the endpoint that clients use. Empty means AWS.
    IcebergRESTCatalogCredentialVendor(
        std::shared_ptr<S3::AWSAssumeRoleClient> sts_client_,
        IcebergRESTCatalogWarehouseCredential credential_,
        String s3_endpoint_,
        std::shared_ptr<const S3::Client> s3_client_);

    /// Reuses the credentials of the same location and access for the first half of their lifetime.
    IcebergRESTCatalogVendedCredentials vend(const String & location, const String & bucket, const String & key, IcebergRESTCatalogVendedAccess access) const;

private:
    struct CacheKey
    {
        String location;
        IcebergRESTCatalogVendedAccess access;
        auto operator<=>(const CacheKey &) const = default;
    };

    struct CacheEntry
    {
        IcebergRESTCatalogVendedCredentials credentials;
        Int64 reuse_until_ms = 0;
    };

    const std::shared_ptr<S3::AWSAssumeRoleClient> sts_client;
    const IcebergRESTCatalogWarehouseCredential credential;
    const String s3_endpoint;
    const std::shared_ptr<const S3::Client> s3_client;

    mutable std::mutex mutex;
    mutable std::map<CacheKey, CacheEntry> cache;
};

}
