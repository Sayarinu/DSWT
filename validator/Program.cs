using System.Buffers.Binary;
using Google.Protobuf;
using DSWT.Validator;
using Dswt.V1;

if (args.SequenceEqual(new[] { "--worker" }))
{
    // One isolated worker serves canonical payloads until its input closes:
    // each request is a format byte (0 PK4, 1 PK5) and 136 bytes, and gets a
    // reply framed by its little-endian u16 length. Any other format byte ends it.
    try
    {
        using var input = Console.OpenStandardInput();
        using var output = Console.OpenStandardOutput();
        byte[] data = new byte[Policy.PayloadSize];
        byte[] header = new byte[2];
        while (true)
        {
            int format = input.ReadByte();
            if (format == -1)
                return 0;
            if (!Policy.IsKnown((PokemonFormat)format))
                return 2;
            await input.ReadExactlyAsync(data);
            byte[] reply = Policy.Analyze((PokemonFormat)format, data.ToArray()).ToByteArray();
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
