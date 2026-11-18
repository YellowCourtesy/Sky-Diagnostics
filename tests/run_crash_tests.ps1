# Runs crash_test.exe in each crash mode and checks the report content.
param([Parameter(Mandatory = $true)][string]$BinDir)

$ErrorActionPreference = "Stop"
$BinDir = (Resolve-Path $BinDir).Path
$failed = $false

$cases = @(
	@{ Mode = "mod-av"; Expect = @("EXCEPTION_ACCESS_VIOLATION", "Code used a null pointer", "Most probable source: badmod.dll", "Confidence: high", "(Widget*)", "(main thread)", "Minidump: written", "== GAME THREAD TIME (last 5 s) ==", "== TEST SECTION ==", "section text 42", "== LAST EVENTS", "test event one", "== MEMORY ==", "Most probable source: badmod.dll (mod: Test Mod)", "mod: Test Mod", "== HEAP MEMORY BY MOD DLL ==") },
	@{ Mode = "mod-cpp"; Expect = @("C++ type: std::runtime_error", "C++ message: test message 123", "Most probable source: badmod.dll") },
	@{ Mode = "mod-overflow"; Expect = @("EXCEPTION_STACK_OVERFLOW", "Most probable source: badmod.dll") },
	@{ Mode = "game-av"; Expect = @("EXCEPTION_ACCESS_VIOLATION", "write to address 0x0000123456780000", "The crash occurred in the game executable") },
	@{ Mode = "game-badptr"; Expect = @("EXCEPTION_ACCESS_VIOLATION", "cannot be an address", "The crash occurred in the game executable") },
	@{ Mode = "mod-object"; Expect = @("(FakeRef*) FakeForm id 0xBEEF", "FakeRef: FakeForm id 0xBEEF", "(Holder<Widget>*)", "(Holder<...>*)", "TestPlugin.esp (1 object)", "Most probable source: badmod.dll") },
	@{ Mode = "stall-spin"; Stall = $true; Expect = @("(game update thread)", "is running code", "Most probable source: badmod.dll", "Confidence: high") },
	@{ Mode = "stall-block"; Stall = $true; Expect = @("(game update thread)", "is waiting", "Most probable source: badmod.dll", "NtWaitForSingleObject") }
)

foreach ($case in $cases) {
	$dir = Join-Path $BinDir ("crash-reports-" + $case.Mode)
	Remove-Item -Recurse -Force $dir -ErrorAction SilentlyContinue
	New-Item -ItemType Directory -Force -Path $dir | Out-Null

	Write-Host "=== $($case.Mode) ==="
	$process = Start-Process -FilePath (Join-Path $BinDir "crash_test.exe") -ArgumentList $case.Mode, "`"$dir`"" -Wait -PassThru -NoNewWindow
	Write-Host "exit code: $($process.ExitCode)"
	if ($case.Stall) {
		if ($process.ExitCode -ne 42) {
			Write-Host "FAIL: the stall report was not written"
			$failed = $true
			continue
		}
	} elseif ($process.ExitCode -eq 0 -or $process.ExitCode -eq 2) {
		Write-Host "FAIL: the test program did not crash"
		$failed = $true
		continue
	}

	$filter = if ($case.Stall) { "stall-*.log" } else { "crash-*.log" }
	$log = Get-ChildItem -Path $dir -Filter $filter | Select-Object -First 1
	if (-not $log) {
		Write-Host "FAIL: no crash report"
		$failed = $true
		continue
	}
	$text = Get-Content -Raw $log.FullName
	$caseFailed = $false
	foreach ($needle in $case.Expect) {
		if ($text.Contains($needle)) {
			Write-Host "ok: $needle"
		} else {
			Write-Host "FAIL: the report does not contain: $needle"
			$caseFailed = $true
		}
	}
	if ($caseFailed) {
		$failed = $true
		Write-Host "--- full report ---"
		Write-Host $text
	} else {
		# Show the last section as a sample.
		$index = $text.IndexOf("== PROBABLE CAUSE ==")
		if ($index -ge 0) { Write-Host $text.Substring($index) }
	}
}

# Session record: what the next start reports after each type of end.
function Invoke-Mode([string]$Mode, [string]$Dir) {
	$p = Start-Process -FilePath (Join-Path $BinDir "crash_test.exe") -ArgumentList $Mode, "`"$Dir`"" -Wait -PassThru -NoNewWindow
	return $p.ExitCode
}

function Test-Sequence([string]$Name, [string]$FirstMode, [int]$ExpectNext, [string[]]$Expect) {
	Write-Host "=== session: $Name ==="
	$dir = Join-Path $BinDir ("session-" + $Name)
	Remove-Item -Recurse -Force $dir -ErrorAction SilentlyContinue
	New-Item -ItemType Directory -Force -Path $dir | Out-Null

	$first = Invoke-Mode $FirstMode $dir
	$next = Invoke-Mode "session-next" $dir
	Write-Host "first exit code: $first, next start result: $next (10 = unclean exit reported, 11 = not reported)"
	if ($next -ne $ExpectNext) {
		Write-Host "FAIL: expected $ExpectNext"
		$script:failed = $true
		return
	}
	if ($Expect.Count -gt 0) {
		$log = Get-ChildItem -Path $dir -Filter "unclean-exit-*.log" | Select-Object -First 1
		if (-not $log) {
			Write-Host "FAIL: no unclean exit report"
			$script:failed = $true
			return
		}
		$text = Get-Content -Raw $log.FullName
		foreach ($needle in $Expect) {
			if ($text.Contains($needle)) {
				Write-Host "ok: $needle"
			} else {
				Write-Host "FAIL: the report does not contain: $needle"
				$script:failed = $true
			}
		}
		Write-Host $text.Substring($text.IndexOf("== PROBABLE CAUSE =="))
	}
	# The session that made the report ended correctly: no second report.
	$again = Invoke-Mode "session-next" $dir
	if ($again -ne 11) {
		Write-Host "FAIL: a correct exit was reported as unclean ($again)"
		$script:failed = $true
	}
}

Write-Host "=== memory scan ==="
$scanDir = Join-Path $BinDir "memory-scan"
Remove-Item -Recurse -Force $scanDir -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force -Path $scanDir | Out-Null
$scan = Invoke-Mode "memory-scan" $scanDir
if ($scan -ne 20) {
	Write-Host "FAIL: the address space scan did not find the test memory ($scan)"
	$failed = $true
} else {
	Write-Host "ok: the scan found the 64 MB test block"
}

Write-Host "=== memory by mod DLL ==="
$trackDir = Join-Path $BinDir "memtrack"
Remove-Item -Recurse -Force $trackDir -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force -Path $trackDir | Out-Null
$track = Invoke-Mode "memtrack" $trackDir
if ($track -ne 80) {
	Write-Host "FAIL: the memory count by mod DLL is wrong ($track)"
	$failed = $true
} else {
	Write-Host "ok: memory count by mod DLL"
}

Write-Host "=== profiler ==="
$profileDir = Join-Path $BinDir "profile"
Remove-Item -Recurse -Force $profileDir -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force -Path $profileDir | Out-Null
$profile = Invoke-Mode "profile" $profileDir
if ($profile -ne 40) {
	Write-Host "FAIL: the profiler result is wrong ($profile)"
	$failed = $true
} else {
	Write-Host "ok: the profiler found the work in the test mod and the idle time"
}

Write-Host "=== session report ==="
$reportDir = Join-Path $BinDir "report"
Remove-Item -Recurse -Force $reportDir -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force -Path $reportDir | Out-Null
$report = Invoke-Mode "report" $reportDir
if ($report -ne 70) {
	Write-Host "FAIL: the session report or the comparison is wrong ($report)"
	$failed = $true
} else {
	Write-Host "ok: session report, limit of 10 sessions, comparison"
}

Write-Host "=== settings ==="
$settingsDir = Join-Path $BinDir "settings"
Remove-Item -Recurse -Force $settingsDir -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force -Path $settingsDir | Out-Null
$settings = Invoke-Mode "settings" $settingsDir
if ($settings -ne 60) {
	Write-Host "FAIL: the settings parser is wrong ($settings)"
	$failed = $true
} else {
	Write-Host "ok: settings parser"
}

Write-Host "=== advisor ==="
$advisorDir = Join-Path $BinDir "advisor"
Remove-Item -Recurse -Force $advisorDir -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force -Path $advisorDir | Out-Null
$advisor = Invoke-Mode "advisor" $advisorDir
if ($advisor -ne 50) {
	Write-Host "FAIL: the warning rules are wrong ($advisor)"
	$failed = $true
} else {
	Write-Host "ok: warning rules"
}

Write-Host "=== frame statistics ==="
$statsDir = Join-Path $BinDir "frame-stats"
Remove-Item -Recurse -Force $statsDir -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force -Path $statsDir | Out-Null
$stats = Invoke-Mode "frame-stats" $statsDir
if ($stats -ne 30) {
	Write-Host "FAIL: frame statistics are wrong ($stats)"
	$failed = $true
} else {
	Write-Host "ok: frame statistics and limit classification"
}

Test-Sequence "killed" "session-kill" 10 @("test event one", "no freeze and no memory problem", "Confidence: low")
Test-Sequence "frozen-killed" "session-frozen-kill" 10 @("did not respond for 45 s", "The game was frozen")
Test-Sequence "crashed" "mod-av" 11 @()
Test-Sequence "clean" "session-next" 11 @()

if ($failed) { exit 1 }
Write-Host "All crash tests passed."
