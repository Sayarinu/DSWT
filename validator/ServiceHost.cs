using System.Net;
using Microsoft.AspNetCore.Server.Kestrel.Core;

namespace PlatinumWT.Validator;

public static class ServiceHost
{
    public static WebApplication Build(int port = 50052)
    {
        var builder = WebApplication.CreateBuilder(new WebApplicationOptions { Args = [] });
        builder.Configuration.Sources.Clear();
        // Private sidecar endpoint: never bind an externally reachable address.
        builder.WebHost.ConfigureKestrel(options =>
        {
            options.Listen(IPAddress.Loopback, port, endpoint => endpoint.Protocols = HttpProtocols.Http2);
            options.Limits.MaxRequestBodySize = 512;
            options.Limits.MaxRequestHeadersTotalSize = 4096;
            options.Limits.MaxConcurrentConnections = 16;
            options.Limits.Http2.MaxStreamsPerConnection = 4;
            options.Limits.RequestHeadersTimeout = TimeSpan.FromSeconds(5);
        });
        builder.Logging.ClearProviders();
        builder.Logging.AddSimpleConsole();
        builder.Logging.SetMinimumLevel(LogLevel.Warning);
        builder.Services.AddSingleton<WorkerRunner>();
        builder.Services.AddGrpc(options =>
        {
            options.MaxReceiveMessageSize = 256;
            options.MaxSendMessageSize = 1024;
            options.EnableDetailedErrors = false;
            options.CompressionProviders.Clear();
        });
        var app = builder.Build();
        app.MapGrpcService<LegalityService>();
        return app;
    }
}
