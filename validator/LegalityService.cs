using System.Security.Cryptography;
using Grpc.Core;
using Platinumwt.V1;

namespace PlatinumWT.Validator;

public sealed class LegalityService(WorkerRunner worker) : PokemonLegality.PokemonLegalityBase
{
    public override async Task<ValidateReply> Validate(ValidateRequest request, ServerCallContext context)
    {
        var pokemon = request.Pokemon;
        if (pokemon is null || pokemon.Pk4.Length != Policy.PayloadSize || pokemon.Sha256.Length != 32 ||
            !CryptographicOperations.FixedTimeEquals(SHA256.HashData(pokemon.Pk4.Span), pokemon.Sha256.Span))
            throw new RpcException(new Status(StatusCode.InvalidArgument, "invalid canonical payload"));
        return await worker.Run(pokemon.Pk4.ToByteArray(), context.CancellationToken);
    }
}
