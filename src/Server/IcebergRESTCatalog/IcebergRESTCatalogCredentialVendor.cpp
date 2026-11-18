#include <Server/IcebergRESTCatalog/IcebergRESTCatalogCredentialVendor.h>

#include "config.h"

#include <Common/Exception.h>
#include <IO/WriteHelpers.h>
#include <Server/IcebergRESTCatalog/IcebergRESTCatalogJSON.h>

#include <Poco/JSON/Array.h>
#include <Poco/JSON/Object.h>

#if USE_AWS_S3
#include <IO/S3/Client.h>
#include <IO/S3/Credentials.h>

#include <aws/core/Region.h>
#endif

namespace DB
{

using namespace IcebergRESTCatalog;

namespace ErrorCodes
{
    extern const int BAD_ARGUMENTS;
    extern const int S3_ERROR;
    extern const int SUPPORT_IS_DISABLED;
}

namespace
{

#if USE_AWS_S3
/// The partition of `arn:<partition>:iam::<account>:role/<name>`, for example `aws`, `aws-cn` or `aws-us-gov`.
String getPartition(const String & role_arn)
{
    static constexpr std::string_view arn_prefix = "arn:";
    const auto end = role_arn.find(':', arn_prefix.size());
    if (!role_arn.starts_with(arn_prefix) || end == String::npos || end == arn_prefix.size())
        throw Exception(ErrorCodes::BAD_ARGUMENTS, "Role {} is not an ARN", role_arn);
    return role_arn.substr(arn_prefix.size(), end - arn_prefix.size());
}

/// A session policy that limits the assumed role to one table prefix.
/// `ListBucket` is a bucket-level action, so it needs an explicit prefix condition.
/// The bucket is in the partition of the role.
/// TODO: A bucket with SSE-KMS also needs `kms:Decrypt` and `kms:GenerateDataKey`. A session policy without them denies the KMS calls.
/// TODO: STS limits a session policy to 2048 characters. The key is in the policy two times, so a long key fails.
/// TODO: IAM reads `*` and `?` in the key as wildcards and has no escape for them. The uuid at the end of a table location keeps the match inside one table.
String buildSessionPolicy(const String & partition, const String & bucket, const String & key, IcebergRESTCatalogVendedAccess access)
{
    const auto bucket_arn = fmt::format("arn:{}:s3:::{}", partition, bucket);
    const auto prefix = key.empty() ? String() : key + "/";

    Poco::JSON::Array object_actions;
    object_actions.add("s3:GetObject");
    if (access == IcebergRESTCatalogVendedAccess::ReadWrite)
    {
        object_actions.add("s3:PutObject");
        object_actions.add("s3:DeleteObject");
        object_actions.add("s3:AbortMultipartUpload");
    }
    Poco::JSON::Object object_statement;
    object_statement.set("Effect", "Allow");
    object_statement.set("Action", object_actions);
    object_statement.set("Resource", fmt::format("{}/{}*", bucket_arn, prefix));

    Poco::JSON::Object prefix_condition;
    prefix_condition.set("s3:prefix", fmt::format("{}*", prefix));
    Poco::JSON::Object condition;
    condition.set("StringLike", prefix_condition);
    Poco::JSON::Object list_statement;
    list_statement.set("Effect", "Allow");
    list_statement.set("Action", "s3:ListBucket");
    list_statement.set("Resource", bucket_arn);
    list_statement.set("Condition", condition);

    /// Clients call `GetBucketLocation` to find the region before the first read.
    Poco::JSON::Object location_statement;
    location_statement.set("Effect", "Allow");
    location_statement.set("Action", "s3:GetBucketLocation");
    location_statement.set("Resource", bucket_arn);

    Poco::JSON::Array statements;
    statements.add(object_statement);
    statements.add(list_statement);
    statements.add(location_statement);

    Poco::JSON::Object policy;
    policy.set("Version", "2012-10-17");
    policy.set("Statement", statements);
    return toJSONString(policy);
}
#endif

}

IcebergRESTCatalogCredentialVendor::IcebergRESTCatalogCredentialVendor(
    std::shared_ptr<S3::AWSAssumeRoleClient> sts_client_,
    IcebergRESTCatalogWarehouseCredential credential_,
    String s3_endpoint_,
    std::shared_ptr<const S3::Client> s3_client_)
    : sts_client(std::move(sts_client_))
    , credential(std::move(credential_))
    , s3_endpoint(std::move(s3_endpoint_))
    , s3_client(std::move(s3_client_))
{
}

IcebergRESTCatalogVendedCredentials IcebergRESTCatalogCredentialVendor::vend(
    const String & location, const String & bucket, const String & key, IcebergRESTCatalogVendedAccess access) const
{
#if USE_AWS_S3
    const CacheKey cache_key{.location = location, .access = access};
    const auto now_ms = nowMs();

    {
        std::lock_guard lock(mutex);
        if (const auto it = cache.find(cache_key); it != cache.end() && it->second.reuse_until_ms > now_ms)
            return it->second.credentials;
    }

    /// Concurrent misses for one key each call STS. The last one wins the cache slot. All of them are valid.
    /// TODO: `DurationSeconds` is not sent. STS uses the default of one hour.
    /// The S3 client of the warehouse uses the same default session name.
    const auto session_name = credential.role_session_name.empty() ? "ClickHouseSession" : credential.role_session_name;
    S3::AssumeRoleRequest request(
        Aws::String(credential.role_arn),
        Aws::String(session_name),
        Aws::String(credential.external_id),
        Aws::String(buildSessionPolicy(getPartition(credential.role_arn), bucket, key, access)));
    const auto outcome = sts_client->assumeRole(request);
    if (!outcome.IsSuccess())
        throw Exception(
            ErrorCodes::S3_ERROR,
            "Cannot assume role {} to vend credentials for {}: {}",
            credential.role_arn,
            location,
            outcome.GetError().GetMessage());

    const auto & result = outcome.GetResult();
    if (result.getAccessKeyID().empty() || result.getSecretAccessKey().empty())
        throw Exception(
            ErrorCodes::S3_ERROR, "STS returned no credentials for role {} to vend credentials for {}", credential.role_arn, location);

    IcebergRESTCatalogVendedCredentials credentials;
    credentials.prefix = location;
    /// TODO: A response without `Expiration` gives 0 here. The client then reads the credentials as expired.
    credentials.expires_at_ms = result.getExpiration().Millis();
    credentials.config["s3.access-key-id"] = result.getAccessKeyID();
    credentials.config["s3.secret-access-key"] = result.getSecretAccessKey();
    /// AWS always returns a token. Some S3-compatible stores return none.
    if (!result.getSessionToken().empty())
        credentials.config["s3.session-token"] = result.getSessionToken();
    credentials.config["s3.session-token-expires-at-ms"] = toString(credentials.expires_at_ms);
    if (!s3_endpoint.empty())
    {
        /// A custom endpoint is not AWS, so clients must address the bucket in the path.
        credentials.config["s3.endpoint"] = s3_endpoint;
        credentials.config["s3.path-style-access"] = "true";
    }

    auto region = s3_client->getRegionForBucket(bucket);
    if (region.empty())
        region = s3_client->getRegion();
    if (!region.empty() && region != Aws::Region::AWS_GLOBAL)
    {
        credentials.config["s3.region"] = region;
        /// Spark reads this key.
        credentials.config["client.region"] = region;
    }

    /// Credentials are reused for the first half of their lifetime, so a client always gets the other half.
    const auto reuse_until_ms = now_ms + (credentials.expires_at_ms - now_ms) / 2;

    std::lock_guard lock(mutex);
    std::erase_if(cache, [&](const auto & entry) { return entry.second.reuse_until_ms <= now_ms; });
    cache[cache_key] = CacheEntry{.credentials = credentials, .reuse_until_ms = reuse_until_ms};
    return credentials;
#else
    UNUSED(location, bucket, key, access);
    throw Exception(ErrorCodes::SUPPORT_IS_DISABLED, "Credential vending needs S3 support, but this build has none");
#endif
}

}
