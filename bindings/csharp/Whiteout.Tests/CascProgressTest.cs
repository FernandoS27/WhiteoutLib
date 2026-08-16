// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Fernando Sahmkow

using Whiteout.Casc;
using Xunit;

namespace Whiteout.Tests;

/// <summary>
/// Smoke tests for the CASC progress shims. A real CASC install is too large
/// to ship in-tree, so these drive the trampoline over a directory that fails
/// to open — which still reports the first step before failing, and is enough
/// to prove events cross the boundary intact.
/// </summary>
public sealed class CascProgressTest
{
    [Fact]
    public void StepName_IsNonEmptyForEveryStep()
    {
        foreach (CascProgressStep step in Enum.GetValues<CascProgressStep>())
        {
            var name = StorageProgress.StepName(step);
            Assert.False(string.IsNullOrWhiteSpace(name), $"no label for {step}");
        }

        Assert.Equal("Loading index files", StorageProgress.StepName(CascProgressStep.LoadingIndexFiles));
    }

    [Fact]
    public void OpenLocal_ReportsEventsBeforeFailing()
    {
        var bogus = Path.Combine(Path.GetTempPath(), $"whiteout-progress-{Guid.NewGuid():N}");
        Directory.CreateDirectory(bogus);
        try
        {
            var events = new List<(CascProgressStep Step, CascProgressState State, string Object)>();
            using var storage = StorageProgress.OpenLocal(bogus, (in CascProgressInfo info) =>
            {
                events.Add((info.Step, info.State, info.Object));
                Assert.True(info.OverallFraction is >= 0.0 and <= 1.0);
                Assert.True(info.StepCount > 0);
                Assert.NotNull(info.Object);
                return true;
            });

            // The open fails (no .idx files), but the step it failed in was
            // announced first — that announcement is what a UI shows.
            Assert.Null(storage);
            Assert.NotEmpty(events);
            Assert.Contains(events, e => e.Step == CascProgressStep.LoadingIndexFiles);
        }
        finally
        {
            Directory.Delete(bogus, recursive: true);
        }
    }

    [Fact]
    public void OpenLocal_HandlerCanCancel()
    {
        var bogus = Path.Combine(Path.GetTempPath(), $"whiteout-progress-{Guid.NewGuid():N}");
        Directory.CreateDirectory(bogus);
        try
        {
            int calls = 0;
            using var storage = StorageProgress.OpenLocal(bogus, (in CascProgressInfo info) =>
            {
                calls++;
                return false;
            });

            Assert.Null(storage);
            Assert.Equal(1, calls); // cancelling stops the event stream
        }
        finally
        {
            Directory.Delete(bogus, recursive: true);
        }
    }

    [Fact]
    public void OpenLocal_WithoutHandler_StillWorks()
    {
        var bogus = Path.Combine(Path.GetTempPath(), $"whiteout-progress-{Guid.NewGuid():N}");
        Directory.CreateDirectory(bogus);
        try
        {
            using var storage = StorageProgress.OpenLocal(bogus);
            Assert.Null(storage);
        }
        finally
        {
            Directory.Delete(bogus, recursive: true);
        }
    }
}
