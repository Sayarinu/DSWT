using System.Net;
using Microsoft.AspNetCore.Server.Kestrel.Core;

namespace DSWT.Validator;

public static class ServiceHost
{
    private const int DefaultPort = 50052;
    private const int MaxRequestBodyBytes = 512;
    private const int MaxRequestHeaderBytes = 4096;
    private const int MaxConnections = 16;
    private const int MaxStreams = 4;
    private const int MaxGrpcRequestBytes = 256;
    private static readonly TimeSpan HeaderTimeout = TimeSpan.FromSeconds(5);

    public static WebApplication Build(int port = DefaultPort)
    {
        var builder = WebApplication.CreateBuilder(new WebApplicationOptions { Args = [] });
        builder.Configuration.Sources.Clear();
        // Private sidecar endpoint: never bind an externally reachable address.
        builder.WebHost.ConfigureKestrel(options =>
        {
            options.Listen(IPAddress.Loopback, port, endpoint => endpoint.Protocols = HttpProtocols.Http2);
            options.Limits.MaxRequestBodySize = MaxRequestBodyBytes;
            options.Limits.MaxRequestHeadersTotalSize = MaxRequestHeaderBytes;
            options.Limits.MaxConcurrentConnections = MaxConnections;
            options.Limits.Http2.MaxStreamsPerConnection = MaxStreams;
            options.Limits.RequestHeadersTimeout = HeaderTimeout;
        });
        builder.Logging.ClearProviders();
        builder.Logging.AddSimpleConsole();
        builder.Logging.SetMinimumLevel(LogLevel.Warning);
        builder.Services.AddSingleton<WorkerRunner>();
        builder.Services.AddGrpc(options =>
        {
            options.MaxReceiveMessageSize = MaxGrpcRequestBytes;
            options.MaxSendMessageSize = WorkerRunner.MaxReplyBytes;
            options.EnableDetailedErrors = false;
            options.CompressionProviders.Clear();
        });
        var app = builder.Build();
        app.MapGrpcService<LegalityService>();
        return app;
    }
}
