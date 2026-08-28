// Copyright (c) 2026 RidgeGlow
//
// Licensed under the Functional Source License 1.1 (MIT future). See LICENSE.

using System;
using System.IO;
using System.Reflection;
using System.Security.Cryptography;
using System.Threading;
using UnrealBuildTool;

/// <summary>
/// External module exposing the libgitlfs C API.
///
/// This module deliberately does NOT link anything. It publishes the vendored
/// header and, when it can, stages a prebuilt shared library into the plugin's
/// Binaries directory for the runtime loader to pick up with GetDllHandle.
///
/// The consequence is the property this whole design rests on: a build can never
/// fail because the download failed, because there is nothing to link against.
/// A missing library is a runtime condition, reported loudly at load, not a
/// broken compile for someone who cloned the repo behind a firewall.
/// </summary>
public class GitLfsLib : ModuleRules
{
	public GitLfsLib(ReadOnlyTargetRules Target) : base(Target)
	{
		Type = ModuleType.External;

		// Always available: the header is vendored, so the C++ compiles whatever
		// happens below.
		PublicIncludePaths.Add(Path.Combine(ModuleDirectory, "include"));

		string LibFileName = GitLfsLibAcquire.Acquire(Target, ModuleDirectory);
		if (string.IsNullOrEmpty(LibFileName))
		{
			return;
		}

		// Stage the library next to the plugin's other binaries. The two-argument
		// form copies source -> target as part of the build, which is what puts
		// it where the runtime loader looks.
		string StagedPath = Path.Combine("$(PluginDir)", "Binaries", Target.Platform.ToString(), LibFileName);
		string SourcePath = Path.Combine(GitLfsLibAcquire.LibDir(ModuleDirectory, Target), LibFileName);
		RuntimeDependencies.Add(StagedPath, SourcePath);
	}
}

/// <summary>
/// Downloads the pinned libgitlfs shared library.
///
/// Everything here is constrained by one fact: UnrealBuildTool compiles .Build.cs
/// against a fixed, small set of reference assemblies that does NOT include
/// System.Net.Http or System.IO.Compression, and that set is older still on
/// UE 4.27 (where UBT runs on .NET Framework rather than .NET 8/10).
///
/// Hence two deliberate choices:
///   - The release ships raw files, never archives, so no decompressor is needed.
///   - HTTP goes through reflection, so the rules assembly compiles regardless of
///     what is on the reference list.
///
/// Nothing in here may throw. Every failure path warns and returns null.
/// </summary>
internal static class GitLfsLibAcquire
{
	private const string ReleaseUrlFormat =
		"https://github.com/RidgeGlow/git-lfs-lib/releases/download/{0}/{1}";

	private const string NotPinnedMarker = "PENDING_RELEASE";
	private const int DownloadTimeoutSeconds = 60;
	private const int LockTimeoutSeconds = 120;

	public static string LibDir(string ModuleDirectory, ReadOnlyTargetRules Target)
	{
		return Path.Combine(ModuleDirectory, "Lib", PlatformKey(Target));
	}

	/// <returns>The library file name if it is present on disk, otherwise null.</returns>
	public static string Acquire(ReadOnlyTargetRules Target, string ModuleDirectory)
	{
		try
		{
			string Key = PlatformKey(Target);
			if (Key == null)
			{
				// Not a host platform the editor runs on; nothing to do and nothing to warn about.
				return null;
			}

			string Tag, FileName, ExpectedHash;
			if (!ReadManifest(ModuleDirectory, Key, out Tag, out FileName, out ExpectedHash))
			{
				return null;
			}

			string TargetDir = Path.Combine(ModuleDirectory, "Lib", Key);
			string TargetFile = Path.Combine(TargetDir, FileName);

			// An explicit local build always wins, so a developer working on the
			// library itself is never fighting the downloader.
			string LocalDir = Environment.GetEnvironmentVariable("GITLFSLIB_LOCAL_DIR");
			if (!string.IsNullOrEmpty(LocalDir))
			{
				string LocalFile = Path.Combine(LocalDir, FileName);
				if (File.Exists(LocalFile))
				{
					Directory.CreateDirectory(TargetDir);
					File.Copy(LocalFile, TargetFile, true);
					Console.WriteLine("GitLfsLib: using local library from {0}", LocalFile);
					return FileName;
				}
				Warn(string.Format("GITLFSLIB_LOCAL_DIR is set but {0} does not exist.", LocalFile));
			}

			if (IsStampCurrent(TargetDir, Tag, ExpectedHash) && File.Exists(TargetFile))
			{
				return FileName;
			}

			if (ExpectedHash == NotPinnedMarker || string.IsNullOrEmpty(ExpectedHash))
			{
				Warn(string.Format(
					"libgitlfs is not pinned yet for '{0}' ({1} has no SHA-256 for it), so it was not downloaded. " +
					"The plugin will build and run, falling back to invoking git-lfs as a process.",
					Key, "GitLfsLib.deps.txt"));
				return File.Exists(TargetFile) ? FileName : null;
			}

			if (IsTruthy(Environment.GetEnvironmentVariable("GITLFSLIB_SKIP_DOWNLOAD")))
			{
				if (File.Exists(TargetFile))
				{
					return FileName;
				}
				Warn("GITLFSLIB_SKIP_DOWNLOAD is set and no library is present; the plugin will use the process fallback.");
				return null;
			}

			string Url = string.Format(ReleaseUrlFormat, Tag, FileName);
			if (DownloadVerified(Url, TargetDir, TargetFile, ExpectedHash))
			{
				WriteStamp(TargetDir, Tag, ExpectedHash);
				Console.WriteLine("GitLfsLib: downloaded {0}", FileName);
				return FileName;
			}

			// Download failed. A stale-but-present library is still better than none.
			return File.Exists(TargetFile) ? FileName : null;
		}
		catch (Exception Ex)
		{
			// Belt and braces: this method is not permitted to break a build.
			Warn("unexpected failure while acquiring libgitlfs: " + Ex.Message);
			return null;
		}
	}

	private static string PlatformKey(ReadOnlyTargetRules Target)
	{
		if (Target.Platform == UnrealTargetPlatform.Win64)
		{
			return "win64";
		}
		if (Target.Platform == UnrealTargetPlatform.Mac)
		{
			return "mac";
		}
		if (Target.Platform == UnrealTargetPlatform.Linux)
		{
			return "linux";
		}
		return null;
	}

	private static bool ReadManifest(string ModuleDirectory, string Key,
		out string Tag, out string FileName, out string Hash)
	{
		Tag = null;
		FileName = null;
		Hash = null;

		string ManifestPath = Path.Combine(ModuleDirectory, "GitLfsLib.deps.txt");
		if (!File.Exists(ManifestPath))
		{
			Warn("GitLfsLib.deps.txt is missing; cannot determine which libgitlfs release to use.");
			return false;
		}

		foreach (string RawLine in File.ReadAllLines(ManifestPath))
		{
			string Line = RawLine.Trim();
			if (Line.Length == 0 || Line.StartsWith("#"))
			{
				continue;
			}

			string[] Fields = Line.Split(new char[] { ' ', '\t' }, StringSplitOptions.RemoveEmptyEntries);
			if (Fields.Length >= 2 && Fields[0] == "tag")
			{
				Tag = Fields[1];
			}
			else if (Fields.Length >= 3 && Fields[0] == Key)
			{
				FileName = Fields[1];
				Hash = Fields[2];
			}
		}

		if (string.IsNullOrEmpty(Tag) || string.IsNullOrEmpty(FileName))
		{
			Warn(string.Format("GitLfsLib.deps.txt has no usable entry for platform '{0}'.", Key));
			return false;
		}
		return true;
	}

	private static bool DownloadVerified(string Url, string TargetDir, string TargetFile, string ExpectedHash)
	{
		Directory.CreateDirectory(TargetDir);

		// UBT may compile rules for several targets at once. Without this, two
		// invocations can race and leave a half-written library on disk.
		string LockPath = Path.Combine(TargetDir, ".lock");
		FileStream Lock = AcquireLock(LockPath);
		if (Lock == null)
		{
			Warn("timed out waiting for another build to finish downloading libgitlfs.");
			return false;
		}

		try
		{
			// Another process may have completed the download while we waited.
			if (File.Exists(TargetFile) && HashFile(TargetFile) == ExpectedHash)
			{
				return true;
			}

			string Error;
			byte[] Payload = Download(Url, out Error);
			if (Payload == null)
			{
				Warn(string.Format(
					"could not download libgitlfs from {0} ({1}). The plugin will build and run, " +
					"falling back to invoking git-lfs as a process. To fix this, either restore network " +
					"access, or download the file manually and place it at {2}.",
					Url, Error, TargetFile));
				return false;
			}

			string ActualHash = HashBytes(Payload);
			if (!string.Equals(ActualHash, ExpectedHash, StringComparison.OrdinalIgnoreCase))
			{
				Warn(string.Format(
					"libgitlfs downloaded from {0} does not match its pinned checksum " +
					"(expected {1}, got {2}); it was discarded.", Url, ExpectedHash, ActualHash));
				return false;
			}

			// Write to a temporary file and move into place, so a failure part way
			// through cannot leave a truncated library that then fails to load.
			string TempFile = TargetFile + ".tmp";
			File.WriteAllBytes(TempFile, Payload);
			if (File.Exists(TargetFile))
			{
				File.Delete(TargetFile);
			}
			File.Move(TempFile, TargetFile);
			return true;
		}
		catch (Exception Ex)
		{
			Warn("failed to store the downloaded libgitlfs: " + Ex.Message);
			return false;
		}
		finally
		{
			Lock.Dispose();
			try
			{
				// Best effort: the lock file has served its purpose and would
				// otherwise be packaged into the shipped plugin. Losing this race
				// with another build costs nothing.
				File.Delete(LockPath);
			}
			catch (Exception)
			{
			}
		}
	}

	private static FileStream AcquireLock(string LockPath)
	{
		DateTime Deadline = DateTime.UtcNow.AddSeconds(LockTimeoutSeconds);
		while (DateTime.UtcNow < Deadline)
		{
			try
			{
				return new FileStream(LockPath, FileMode.OpenOrCreate, FileAccess.ReadWrite, FileShare.None);
			}
			catch (IOException)
			{
				Thread.Sleep(200);
			}
		}
		return null;
	}

	/// <summary>
	/// Fetches a URL without a compile-time reference to any HTTP library.
	/// Tries HttpClient first, then WebClient for older runtimes (UE 4.27's UBT
	/// runs on .NET Framework). Returns null and sets Error on failure.
	/// </summary>
	private static byte[] Download(string Url, out string Error)
	{
		EnableModernTls();

		byte[] Result = TryHttpClient(Url, out Error);
		if (Result != null)
		{
			return Result;
		}

		string FallbackError;
		Result = TryWebClient(Url, out FallbackError);
		if (Result != null)
		{
			Error = null;
			return Result;
		}

		Error = string.Format("{0}; {1}", Error, FallbackError);
		return null;
	}

	private static byte[] TryHttpClient(string Url, out string Error)
	{
		Error = null;
		try
		{
			Type ClientType = Type.GetType("System.Net.Http.HttpClient, System.Net.Http");
			if (ClientType == null)
			{
				Error = "System.Net.Http.HttpClient is unavailable";
				return null;
			}

			object Client = Activator.CreateInstance(ClientType);
			try
			{
				PropertyInfo Timeout = ClientType.GetProperty("Timeout");
				if (Timeout != null)
				{
					Timeout.SetValue(Client, TimeSpan.FromSeconds(DownloadTimeoutSeconds), null);
				}

				MethodInfo GetBytes = ClientType.GetMethod("GetByteArrayAsync", new Type[] { typeof(string) });
				if (GetBytes == null)
				{
					Error = "HttpClient.GetByteArrayAsync(string) is unavailable";
					return null;
				}

				object Task = GetBytes.Invoke(Client, new object[] { Url });
				PropertyInfo ResultProperty = Task.GetType().GetProperty("Result");
				return (byte[])ResultProperty.GetValue(Task, null);
			}
			finally
			{
				IDisposable Disposable = Client as IDisposable;
				if (Disposable != null)
				{
					Disposable.Dispose();
				}
			}
		}
		catch (Exception Ex)
		{
			// Reflection wraps the real failure; unwrap it so the warning is useful.
			Exception Root = Ex;
			while (Root.InnerException != null)
			{
				Root = Root.InnerException;
			}
			Error = "HttpClient: " + Root.Message;
			return null;
		}
	}

	private static byte[] TryWebClient(string Url, out string Error)
	{
		Error = null;
		try
		{
			Type ClientType = Type.GetType("System.Net.WebClient, System")
				?? Type.GetType("System.Net.WebClient, System.Net.WebClient")
				?? Type.GetType("System.Net.WebClient, System.Net.Requests");
			if (ClientType == null)
			{
				Error = "System.Net.WebClient is unavailable";
				return null;
			}

			object Client = Activator.CreateInstance(ClientType);
			try
			{
				MethodInfo DownloadData = ClientType.GetMethod("DownloadData", new Type[] { typeof(string) });
				if (DownloadData == null)
				{
					Error = "WebClient.DownloadData(string) is unavailable";
					return null;
				}
				return (byte[])DownloadData.Invoke(Client, new object[] { Url });
			}
			finally
			{
				IDisposable Disposable = Client as IDisposable;
				if (Disposable != null)
				{
					Disposable.Dispose();
				}
			}
		}
		catch (Exception Ex)
		{
			Exception Root = Ex;
			while (Root.InnerException != null)
			{
				Root = Root.InnerException;
			}
			Error = "WebClient: " + Root.Message;
			return null;
		}
	}

	/// <summary>
	/// .NET Framework 4.6.2 (UE 4.27's UBT) may default to a TLS version GitHub
	/// no longer accepts. Harmless and ignored on modern .NET.
	/// </summary>
	private static void EnableModernTls()
	{
		try
		{
			Type Spm = Type.GetType("System.Net.ServicePointManager, System")
				?? Type.GetType("System.Net.ServicePointManager, System.Net.ServicePoint");
			if (Spm == null)
			{
				return;
			}
			PropertyInfo Protocol = Spm.GetProperty("SecurityProtocol");
			if (Protocol == null)
			{
				return;
			}
			int Current = (int)Protocol.GetValue(null, null);
			const int Tls12 = 3072;
			Protocol.SetValue(null, Current | Tls12, null);
		}
		catch (Exception)
		{
			// Best effort only; a modern runtime negotiates TLS correctly anyway.
		}
	}

	private static string HashBytes(byte[] Data)
	{
		using (SHA256 Hasher = SHA256.Create())
		{
			return ToHex(Hasher.ComputeHash(Data));
		}
	}

	private static string HashFile(string Path)
	{
		using (SHA256 Hasher = SHA256.Create())
		using (FileStream Stream = File.OpenRead(Path))
		{
			return ToHex(Hasher.ComputeHash(Stream));
		}
	}

	private static string ToHex(byte[] Bytes)
	{
		char[] Hex = new char[Bytes.Length * 2];
		for (int I = 0; I < Bytes.Length; I++)
		{
			Hex[I * 2] = "0123456789abcdef"[Bytes[I] >> 4];
			Hex[I * 2 + 1] = "0123456789abcdef"[Bytes[I] & 0xF];
		}
		return new string(Hex);
	}

	private static string StampPath(string TargetDir)
	{
		return Path.Combine(TargetDir, ".stamp");
	}

	private static bool IsStampCurrent(string TargetDir, string Tag, string Hash)
	{
		try
		{
			string Path = StampPath(TargetDir);
			return File.Exists(Path) && File.ReadAllText(Path).Trim() == Tag + "|" + Hash;
		}
		catch (Exception)
		{
			return false;
		}
	}

	private static void WriteStamp(string TargetDir, string Tag, string Hash)
	{
		try
		{
			File.WriteAllText(StampPath(TargetDir), Tag + "|" + Hash);
		}
		catch (Exception)
		{
			// Losing the stamp only costs a redundant download next build.
		}
	}

	private static bool IsTruthy(string Value)
	{
		if (string.IsNullOrEmpty(Value))
		{
			return false;
		}
		return Value != "0"
			&& !Value.Equals("false", StringComparison.OrdinalIgnoreCase)
			&& !Value.Equals("no", StringComparison.OrdinalIgnoreCase);
	}

	/// <summary>
	/// Warnings go through Console.WriteLine rather than UnrealBuildTool's Log
	/// class, which lives in Tools.DotNETCommon on UE4 and EpicGames.Core on UE5.
	/// Console output is version-proof and UBT surfaces it either way.
	/// </summary>
	private static void Warn(string Message)
	{
		Console.WriteLine("Warning: GitLfsLib: {0}", Message);
	}
}
