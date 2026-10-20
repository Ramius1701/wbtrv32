using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Text;

// Opens each Btrieve file through wbtrv32.dll in the current directory (which
// converts it on first open), walks every record with StepFirst/StepNext, and
// prints the record count and how long the open and the walk took.
//   harness.exe [FILE ...]   no files = every *.dat in the current directory
//   DUMP=1  write FILE.dump, one hex line per record
//   RO=1    open read-only
static class Harness
{
    const ushort Open = 0, Close = 1, StepNext = 24, StepFirst = 33;

    [DllImport("wbtrv32.dll", CallingConvention = CallingConvention.StdCall)]
    static extern unsafe int BTRCALL(ushort op, byte* pos, byte* data, uint* len, byte* key, byte keyLen, sbyte keyNum);

    static unsafe int Main(string[] args)
    {
        var files = args.Length > 0
            ? args
            : Directory.GetFiles(".", "*.*").Where(p => p.EndsWith(".dat", StringComparison.OrdinalIgnoreCase)).OrderBy(p => p).ToArray();
        bool dump = Environment.GetEnvironmentVariable("DUMP") == "1";
        sbyte openMode = (sbyte)(Environment.GetEnvironmentVariable("RO") == "1" ? -2 : 0);

        var pos = new byte[128]; var data = new byte[65536]; var key = new byte[256];
        int bad = 0;

        // HOLD=1: open every file first and keep them all open while walking,
        // the way a BBS server holds its files, then close them all.
        if (Environment.GetEnvironmentVariable("HOLD") == "1")
        {
            var handles = new Dictionary<string, byte[]>();
            foreach (var path in files)
            {
                var name = Path.GetFileName(path);
                var handle = new byte[128];
                var k = new byte[256];
                Encoding.ASCII.GetBytes(name + "\0").CopyTo(k, 0);
                uint len = 0;
                int status;
                fixed (byte* hp = handle, dp = data, kp = k)
                    status = BTRCALL(Open, hp, dp, &len, kp, 255, openMode);
                if (status == 0) handles[name] = handle; else { bad++; Console.WriteLine($"FAIL  {name,-14} open status={status}"); }
            }
            foreach (var (name, handle) in handles)
            {
                long records = 0; int s;
                fixed (byte* hp = handle, dp = data, kp = key)
                {
                    uint len = (uint)data.Length;
                    s = BTRCALL(StepFirst, hp, dp, &len, kp, 255, 0);
                    while (s == 0) { records++; len = (uint)data.Length; s = BTRCALL(StepNext, hp, dp, &len, kp, 255, 0); }
                }
                if (s != 9) bad++;
                Console.WriteLine(s == 9 ? $"OK    {name,-14} records={records}" : $"OK    {name,-14} records={records} walk-ended-status={s}");
            }
            foreach (var handle in handles.Values)
                fixed (byte* hp = handle, dp = data, kp = key) { uint len = 0; BTRCALL(Close, hp, dp, &len, kp, 255, 0); }
            Console.WriteLine($"--- {bad} file(s) with problems");
            return 0;
        }
        foreach (var path in files)
        {
            var name = Path.GetFileName(path);
            Array.Clear(pos); Array.Clear(key);
            Encoding.ASCII.GetBytes(name + "\0").CopyTo(key, 0);
            long records = 0; string walk = "";
            var openTime = Stopwatch.StartNew();
            int status;
            double openMs, walkMs = 0;
            fixed (byte* pp = pos, dp = data, kp = key)
            {
                uint len = 0;
                status = BTRCALL(Open, pp, dp, &len, kp, 255, openMode);
                openMs = openTime.Elapsed.TotalMilliseconds;
                if (status == 0)
                {
                    using var dumpFile = dump ? new StreamWriter(name + ".dump") : null;
                    var walkTime = Stopwatch.StartNew();
                    len = (uint)data.Length;
                    int s = BTRCALL(StepFirst, pp, dp, &len, kp, 255, 0);
                    while (s == 0)
                    {
                        records++;
                        dumpFile?.WriteLine(Convert.ToHexString(data, 0, (int)len));
                        len = (uint)data.Length;
                        s = BTRCALL(StepNext, pp, dp, &len, kp, 255, 0);
                    }
                    walkMs = walkTime.Elapsed.TotalMilliseconds;
                    walk = s == 9 ? "" : $" walk-ended-status={s}";
                    len = 0;
                    BTRCALL(Close, pp, dp, &len, kp, 255, 0);
                }
            }
            if (status != 0 || walk != "") bad++;
            Console.WriteLine(status == 0
                ? $"OK    {name,-14} records={records}{walk}  open={openMs:F0}ms walk={walkMs:F0}ms"
                : $"FAIL  {name,-14} open status={status}");
        }
        Console.WriteLine($"--- {bad} file(s) with problems");
        return 0;
    }
}
