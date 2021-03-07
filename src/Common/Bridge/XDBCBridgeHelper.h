#pragma once

#include <IO/ReadHelpers.h>
#include <IO/ReadWriteBufferFromHTTP.h>
#include <Interpreters/Context.h>
#include <Access/AccessType.h>
#include <Parsers/IdentifierQuotingStyle.h>
#include <Poco/File.h>
#include <Poco/Logger.h>
#include <Poco/Net/HTTPRequest.h>
#include <Poco/Path.h>
#include <Poco/URI.h>
#include <Poco/Util/AbstractConfiguration.h>
#include <Common/ShellCommand.h>
#include <IO/ConnectionTimeoutsContext.h>
#include <common/logger_useful.h>
#include <ext/range.h>
#include <Common/Bridge/IBridgeHelper.h>

#if !defined(ARCADIA_BUILD)
#    include <Common/config.h>
#endif


namespace DB
{

namespace ErrorCodes
{
    extern const int ILLEGAL_TYPE_OF_ARGUMENT;
}

/// Class for Helpers for XDBC-bridges, provide utility methods, not main request.
class IXDBCBridgeHelper : public IBridgeHelper
{

public:
    virtual std::vector<std::pair<std::string, std::string>> getURLParams(const std::string & cols, UInt64 max_block_size) const = 0;

    virtual Poco::URI getColumnsInfoURI() const = 0;

    virtual IdentifierQuotingStyle getIdentifierQuotingStyle() = 0;

    virtual bool isSchemaAllowed() = 0;

    virtual const String getName() const = 0;
};

using BridgeHelperPtr = std::shared_ptr<IXDBCBridgeHelper>;


template <typename BridgeHelperMixin>
class XDBCBridgeHelper : public IXDBCBridgeHelper
{

public:
    static constexpr inline auto DEFAULT_PORT = BridgeHelperMixin::DEFAULT_PORT;
    static constexpr inline auto COL_INFO_HANDLER = "/columns_info";
    static constexpr inline auto IDENTIFIER_QUOTE_HANDLER = "/identifier_quote";
    static constexpr inline auto SCHEMA_ALLOWED_HANDLER = "/schema_allowed";

    XDBCBridgeHelper(
            const Context & global_context_,
            const Poco::Timespan & http_timeout_,
            const std::string & connection_string_)
    : log(&Poco::Logger::get(BridgeHelperMixin::getName() + "BridgeHelper"))
    , connection_string(connection_string_)
    , http_timeout(http_timeout_)
    , context(global_context_)
    , config(context.getConfigRef())
{
    bridge_host = config.getString(BridgeHelperMixin::configPrefix() + ".host", DEFAULT_HOST);
    bridge_port = config.getUInt(BridgeHelperMixin::configPrefix() + ".port", DEFAULT_PORT);
}


protected:
    auto getConnectionString() const { return connection_string; }

    const String getName() const override { return BridgeHelperMixin::getName(); }

    size_t getDefaultPort() const override { return DEFAULT_PORT; }

    const String serviceAlias() const override { return BridgeHelperMixin::serviceAlias(); }

    /// Same for odbc and jdbc
    const String serviceFileName() const override { return "clickhouse-odbc-bridge"; }

    const String configPrefix() const override { return BridgeHelperMixin::configPrefix(); }

    const Context & getContext() const override { return context; }

    const Poco::Timespan & getHTTPTimeout() const override { return http_timeout; }

    const Poco::Util::AbstractConfiguration & getConfig() const override { return config; }

    Poco::Logger * getLog() const override { return log; }

    bool startBridgeManually() const override { return BridgeHelperMixin::startBridgeManually(); }

    Poco::URI createBaseURI() const override
    {
        Poco::URI uri;
        uri.setHost(bridge_host);
        uri.setPort(bridge_port);
        uri.setScheme("http");
        return uri;
    }

    void startBridge(std::unique_ptr<ShellCommand> cmd) const override
    {
        context.addXDBCBridgeCommand(std::move(cmd));
    }


private:
    using Configuration = Poco::Util::AbstractConfiguration;

    Poco::Logger * log;
    std::string connection_string;
    const Poco::Timespan & http_timeout;
    std::string bridge_host;
    size_t bridge_port;

    const Context & context;
    const Configuration & config;

    std::optional<IdentifierQuotingStyle> quote_style;
    std::optional<bool> is_schema_allowed;


protected:
    Poco::URI getColumnsInfoURI() const override
    {
        auto uri = createBaseURI();
        uri.setPath(COL_INFO_HANDLER);
        return uri;
    }

    std::vector<std::pair<std::string, std::string>> getURLParams(
            const std::string & cols, UInt64 max_block_size) const override
    {
        std::vector<std::pair<std::string, std::string>> result;

        result.emplace_back("connection_string", connection_string); /// already validated
        result.emplace_back("columns", cols);
        result.emplace_back("max_block_size", std::to_string(max_block_size));

        return result;
    }

    bool isSchemaAllowed() override
    {
        if (!is_schema_allowed.has_value())
        {
            startBridgeSync();

            auto uri = createBaseURI();
            uri.setPath(SCHEMA_ALLOWED_HANDLER);
            uri.addQueryParameter("connection_string", getConnectionString());

            ReadWriteBufferFromHTTP buf(
                uri, Poco::Net::HTTPRequest::HTTP_POST, {}, ConnectionTimeouts::getHTTPTimeouts(context));

            bool res;
            readBoolText(res, buf);
            is_schema_allowed = res;
        }

        return *is_schema_allowed;
    }

    IdentifierQuotingStyle getIdentifierQuotingStyle() override
    {
        if (!quote_style.has_value())
        {
            startBridgeSync();

            auto uri = createBaseURI();
            uri.setPath(IDENTIFIER_QUOTE_HANDLER);
            uri.addQueryParameter("connection_string", getConnectionString());

            ReadWriteBufferFromHTTP buf(
                uri, Poco::Net::HTTPRequest::HTTP_POST, {}, ConnectionTimeouts::getHTTPTimeouts(context));
            std::string character;
            readStringBinary(character, buf);
            if (character.length() > 1)
                throw Exception("Failed to parse quoting style from '" + character + "' for service " + BridgeHelperMixin::serviceAlias(), ErrorCodes::ILLEGAL_TYPE_OF_ARGUMENT);
            else if (character.length() == 0)
                quote_style = IdentifierQuotingStyle::None;
            else if (character[0] == '`')
                quote_style = IdentifierQuotingStyle::Backticks;
            else if (character[0] == '"')
                quote_style = IdentifierQuotingStyle::DoubleQuotes;
            else
                throw Exception("Can not map quote identifier '" + character + "' to enum value", ErrorCodes::ILLEGAL_TYPE_OF_ARGUMENT);
        }

        return *quote_style;
    }
};


struct JDBCBridgeMixin
{
    static constexpr inline auto DEFAULT_PORT = 9019;

    static const String configPrefix()
    {
        return "jdbc_bridge";
    }

    static const String serviceAlias()
    {
        return "clickhouse-jdbc-bridge";
    }

    static const String getName()
    {
        return "JDBC";
    }

    static AccessType getSourceAccessType()
    {
        return AccessType::JDBC;
    }

    static bool startBridgeManually()
    {
        return true;
    }
};


struct ODBCBridgeMixin
{
    static constexpr inline auto DEFAULT_PORT = 9018;

    static const String configPrefix()
    {
        return "odbc_bridge";
    }

    static const String serviceAlias()
    {
        return "clickhouse-odbc-bridge";
    }

    static const String getName()
    {
        return "ODBC";
    }

    static AccessType getSourceAccessType()
    {
        return AccessType::ODBC;
    }

    static bool startBridgeManually()
    {
        return false;
    }
};

}
