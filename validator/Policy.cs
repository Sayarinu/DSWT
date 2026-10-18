using System.Buffers.Binary;
using System.Security.Cryptography;
using Google.Protobuf;
using PKHeX.Core;
using Platinumwt.V1;

namespace PlatinumWT.Validator;

public static class Policy
{
    // Bump for changes to the adapter, core, resources or parsing settings.
    public const string Version = "pkhex-26.08.26-74b88906-pwt2";
    public const int PayloadSize = 136;

    public static ValidateReply Analyze(byte[] data)
    {
        if (data.Length != PayloadSize)
            throw new ArgumentException("payload length");
        var reply = new ValidateReply { PolicyVersion = Version, Sha256 = ByteString.CopyFrom(SHA256.HashData(data)) };
        ValidateReply Reject(string reason)
        {
            reply.ReasonCodes.Add(reason);
            return reply;
        }

        uint sum = 0;
        for (int offset = 8; offset < PayloadSize; offset += 2)
            sum += BinaryPrimitives.ReadUInt16LittleEndian(data.AsSpan(offset));
        if (BinaryPrimitives.ReadUInt16LittleEndian(data.AsSpan(4)) != 0 ||
            BinaryPrimitives.ReadUInt16LittleEndian(data.AsSpan(6)) != (ushort)sum)
            return Reject("INVALID_STRUCTURE");
        // PK4's constructor guesses encrypted input from this reserved field.
        // Require canonical input first so that guess is never taken.
        foreach (int offset in new[] { 0x42, 0x43, 0x5E, 0x63, 0x64, 0x65, 0x66, 0x67 })
            if (data[offset] != 0)
                return Reject("INVALID_STRUCTURE");
        ushort species = BinaryPrimitives.ReadUInt16LittleEndian(data.AsSpan(8));
        if (species is 0 or > 493)
            return Reject("INVALID_SPECIES");

        var pk = new PK4(data.ToArray());
        if (pk.IsEgg)
            return Reject("EGG_NOT_ALLOWED");
        if (ItemStorage4.Mail.Contains((ushort)pk.HeldItem))
            return Reject("MAIL_NOT_ALLOWED");
        // Spiky-eared Pichu cannot be traded to Platinum, even though the
        // shared Gen IV personal-table API recognizes it.
        if ((species == 172 && pk.Form != 0) || !PersonalTable.Pt.IsPresentInGame(species, pk.Form))
            return Reject("INCOMPATIBLE_FORM");

        ParseSettings.Initialize(new LegalitySettings());
        // Gen IV names predate the later consoles' word lists.
        ParseSettings.Settings.WordFilter.DisableWordFilterPastGen = true;
        ParseSettings.ClearActiveTrainer();
        ParseSettings.AllowEraCartGBA = true;
        ParseSettings.AllowEraSwitchGBA = false;
        ParseSettings.Settings.FramePattern.RNGFrameNotFound3 = Severity.Invalid;
        ParseSettings.Settings.FramePattern.RNGFrameNotFound4 = Severity.Invalid;
        var analysis = new LegalityAnalysis(pk);
        if (!analysis.Parsed || !pk.Data[..PayloadSize].SequenceEqual(data))
            throw new InvalidOperationException("analysis unavailable");
        reply.Legal = analysis.Valid;
        if (reply.Legal)
            return reply;

        var reasons = new SortedSet<string>(StringComparer.Ordinal);
        foreach (var result in analysis.Results.Where(r => !r.Valid))
            reasons.Add(result.Identifier switch
            {
                CheckIdentifier.Encounter or CheckIdentifier.Fateful or CheckIdentifier.GameOrigin => "ILLEGAL_ENCOUNTER",
                CheckIdentifier.PID or CheckIdentifier.EC or CheckIdentifier.Shiny or CheckIdentifier.IVs => "ILLEGAL_RNG",
                CheckIdentifier.CurrentMove or CheckIdentifier.RelearnMove => "ILLEGAL_MOVES",
                CheckIdentifier.Form => "INCOMPATIBLE_FORM",
                _ => "ILLEGAL_DATA",
            });
        if (analysis.Info.Moves.Any(m => !m.Valid) || analysis.Info.Relearn.Any(m => !m.Valid))
            reasons.Add("ILLEGAL_MOVES");
        if (reasons.Count == 0)
            reasons.Add("ILLEGAL_DATA");
        reply.ReasonCodes.AddRange(reasons);
        return reply;
    }
}
