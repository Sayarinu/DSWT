using Google.Protobuf;
using DSWT.Validator;

if (args.SequenceEqual(new[] { "--worker" }))
{
    try
    {
        using var input = Console.OpenStandardInput();
        byte[] data = new byte[Policy.PayloadSize];
        await input.ReadExactlyAsync(data);
        if (input.ReadByte() != -1)
            return 2;
        var reply = Policy.Analyze(data);
        await Console.OpenStandardOutput().WriteAsync(reply.ToByteArray());
        return 0;
    }
    catch (Exception) { return 2; }
}
if (args.Length != 0)
    return 2;
var app = ServiceHost.Build();
await app.RunAsync();
return 0;
