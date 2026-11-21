using System.Buffers.Binary;
using Google.Protobuf;
using DSWT.Validator;

if (args.SequenceEqual(new[] { "--worker" }))
{
    // One isolated worker serves canonical payloads until its input closes:
    // each 136-byte request gets a reply framed by its little-endian u16 length.
    try
    {
        using var input = Console.OpenStandardInput();
        using var output = Console.OpenStandardOutput();
        byte[] data = new byte[Policy.PayloadSize];
        byte[] header = new byte[2];
        while (true)
        {
            int first = input.ReadByte();
            if (first == -1)
                return 0;
            data[0] = (byte)first;
            await input.ReadExactlyAsync(data.AsMemory(1));
            byte[] reply = Policy.Analyze(data.ToArray()).ToByteArray();
            BinaryPrimitives.WriteUInt16LittleEndian(header, checked((ushort)reply.Length));
            await output.WriteAsync(header);
            await output.WriteAsync(reply);
            await output.FlushAsync();
        }
    }
    catch (Exception) { return 2; }
}
if (args.Length != 0)
    return 2;
var app = ServiceHost.Build();
await app.RunAsync();
return 0;
