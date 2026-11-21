using System.Buffers.Binary;
using System.Collections.Concurrent;
using System.Diagnostics;
using System.Reflection;
using System.Security.Cryptography;
using Grpc.Core;
using Dswt.V1;

namespace DSWT.Validator;

// The core is synchronous and cannot be interrupted safely within a thread.
// Keep its lifetime, pipe buffers and admission bound inside this module.
// Starting a worker costs far more than an analysis, so a worker that answered
// correctly serves later payloads, up to MaxUses. Any timeout, crash, malformed
// or mismatched reply kills it, and it is never reused.
// Request frame: one format byte (PokemonFormat: 0 PK4, 1 PK5), then the 136-byte payload.
// Reply frame: a little-endian u16 length, then that many ValidateReply bytes.
public sealed class WorkerRunner : IDisposable
{
    internal const int MaxReplyBytes = 1024;
    public const int MaxUses = 256;
    private const int DefaultWorkers = 2;
    private const int MaxReasonCodes = 8;
    private const int MaxReasonCodeLength = 32;
    private static readonly TimeSpan AnalysisBudget = TimeSpan.FromSeconds(1);
    private static readonly TimeSpan CleanupBudget = TimeSpan.FromSeconds(1);

    private readonly SemaphoreSlim slots;
    private readonly ConcurrentBag<Worker> idle = [];
    private readonly Func<ProcessStartInfo> command;
    private readonly TimeSpan budget;

    public WorkerRunner() : this(Command, AnalysisBudget, Workers(Environment.GetEnvironmentVariable("WT_VALIDATOR_WORKERS"))) { }
    public WorkerRunner(Func<ProcessStartInfo> command, TimeSpan budget, int workers = DefaultWorkers)
    {
        this.command = command;
        this.budget = budget;
        slots = new(workers);
    }

    // Two isolated workers by default; deployments sized for more CPU may allow up to eight.
    public static int Workers(string? value) => value switch
    {
        null or "" => DefaultWorkers,
        [>= '1' and <= '8'] => value[0] - '0',
        _ => throw new InvalidOperationException("WT_VALIDATOR_WORKERS must be 1-8"),
    };

    private static ProcessStartInfo Command()
    {
        var start = new ProcessStartInfo("dotnet");
        start.ArgumentList.Add(Assembly.GetExecutingAssembly().Location);
        start.ArgumentList.Add("--worker");
        return start;
    }

    private sealed class Worker(Process process)
    {
        public Process Process { get; } = process;
        public int Uses { get; set; }
    }

    private Worker Start()
    {
        var process = new Process();
        try
        {
            process.StartInfo = command();
            process.StartInfo.UseShellExecute = false;
            process.StartInfo.RedirectStandardInput = true;
            process.StartInfo.RedirectStandardOutput = true;
            process.StartInfo.RedirectStandardError = true;
            process.StartInfo.Environment["DOTNET_EnableDiagnostics"] = "0";
            process.StartInfo.Environment["DOTNET_PROCESSOR_COUNT"] = "2";
            process.StartInfo.Environment["DOTNET_GCHeapHardLimit"] = "8000000"; // 128 MiB/worker.
            if (!process.Start())
                throw new InvalidOperationException();
            // Drain diagnostics for the worker's whole life so a full pipe never blocks it.
            _ = process.StandardError.BaseStream.CopyToAsync(Stream.Null);
            return new Worker(process);
        }
        catch
        {
            process.Dispose();
            throw;
        }
    }

    public Task<ValidateReply> Run(byte[] payload, CancellationToken cancellation) =>
        Run(PokemonFormat.FormatPk4, payload, cancellation);

    public async Task<ValidateReply> Run(PokemonFormat format, byte[] payload, CancellationToken cancellation)
    {
        if (!Policy.IsKnown(format) || payload.Length != Policy.PayloadSize)
            throw new RpcException(new Status(StatusCode.InvalidArgument, "invalid canonical payload"));
        var allowed = (uint)Policy.DestinationsFor(format);
        if (!await slots.WaitAsync(0, cancellation))
            throw new RpcException(new Status(StatusCode.ResourceExhausted, "validator busy"));
        using var deadline = CancellationTokenSource.CreateLinkedTokenSource(cancellation);
        deadline.CancelAfter(budget);
        Worker? worker = null;
        bool reusable = false;
        try
        {
            while (worker is null && idle.TryTake(out var warm))
            {
                if (warm.Process.HasExited)
                    await Stop(warm);
                else
                    worker = warm;
            }
            worker ??= Start();
            worker.Uses++;
            var input = worker.Process.StandardInput.BaseStream;
            await input.WriteAsync(new[] { (byte)format }, deadline.Token);
            await input.WriteAsync(payload, deadline.Token);
            await input.FlushAsync(deadline.Token);
            var reply = ValidateReply.Parser.ParseFrom(await ReadFrame(worker.Process.StandardOutput.BaseStream, deadline.Token));
            if (reply.PolicyVersion != Policy.Version ||
                !reply.Sha256.Span.SequenceEqual(SHA256.HashData(payload)) ||
                reply.Legal != (reply.ReasonCodes.Count == 0) || reply.ReasonCodes.Count > MaxReasonCodes ||
                reply.Legal != (reply.Destinations != 0) || (reply.Destinations & ~allowed) != 0 ||
                reply.ReasonCodes.Any(r => r.Length is 0 or > MaxReasonCodeLength || r.Any(c => c is not (>= 'A' and <= 'Z' or '_'))))
                throw new InvalidOperationException();
            reusable = worker.Uses < MaxUses;
            return reply;
        }
        catch (Exception)
        {
            // Never serialize core exception messages, payloads or pipe output.
            throw new RpcException(new Status(cancellation.IsCancellationRequested ? StatusCode.Cancelled : StatusCode.Unavailable,
                "validation unavailable"));
        }
        finally
        {
            if (worker is not null)
            {
                if (reusable)
                    idle.Add(worker);
                else
                    await Stop(worker);
            }
            slots.Release();
        }
    }

    // One reply frame: a little-endian u16 length, then that many bytes, within the bound.
    private static async Task<byte[]> ReadFrame(Stream stream, CancellationToken cancellation)
    {
        byte[] header = new byte[2];
        await stream.ReadExactlyAsync(header, cancellation);
        int length = BinaryPrimitives.ReadUInt16LittleEndian(header);
        if (length > MaxReplyBytes)
            throw new InvalidOperationException("worker output limit");
        byte[] reply = new byte[length];
        await stream.ReadExactlyAsync(reply, cancellation);
        return reply;
    }

    private static async Task Stop(Worker worker)
    {
        var process = worker.Process;
        try
        {
            if (!process.HasExited)
                process.Kill(entireProcessTree: true);
            using var cleanup = new CancellationTokenSource(CleanupBudget);
            await process.WaitForExitAsync(cleanup.Token);
        }
        catch (InvalidOperationException) { }
        catch (OperationCanceledException) { }
        finally { process.Dispose(); }
    }

    public void Dispose()
    {
        while (idle.TryTake(out var worker))
            Stop(worker).GetAwaiter().GetResult();
        slots.Dispose();
    }
}
