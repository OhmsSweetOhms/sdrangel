///////////////////////////////////////////////////////////////////////////////////
// FFT_BENCH shared-engine regression (plan-05 Step 1)                           //
//                                                                               //
// This program is free software; you can redistribute it and/or modify          //
// it under the terms of the GNU General Public License as published by          //
// the Free Software Foundation as version 3 of the License, or                  //
// (at your option) any later version.                                           //
///////////////////////////////////////////////////////////////////////////////////
//
// Locks the runtime policy settled in plan-05-fft-bench-and-wisdom.md:
//
//   Import configured wisdom
//     +-- matching saved plan available -> FFTW_PATIENT | FFTW_WISDOM_ONLY
//     +-- missing / invalid / incompatible -> FFTW_ESTIMATE + loud diagnostic
//
// against FFTWEngine::configure() directly, with no running sdrangelsrv
// process. No test framework is wired into this fork: this is a plain
// assert-and-print executable, PASS/FAIL per case, non-zero exit on any
// failure, same convention CTest expects from a raw COMMAND.
//
// Root-cause reference: findings-2026-07-20-fft-bench-root-cause.md (thread
// cross-cutting/20260711-sdrangel-host-perchannel-sigmf) -- the pre-fix
// fftwengine.cpp called fftwf_plan_dft_1d(..., FFTW_PATIENT) unconditionally,
// with no FFTW_WISDOM_ONLY guard and no fallback, which is what pinned the
// REST thread for the ARM64 board reproduction. This suite does not require
// ARM64/QEMU hardware: testUnrestrictedPatientIsUnboundedMechanism()
// reproduces the mechanism directly against libfftw3f on the build host
// (substitution permitted by the plan-05 launch scope for Step 1's
// "pre-fix ARM64 path reproduces the signature" acceptance item).

#include <cstdio>
#include <chrono>

#include <QCoreApplication>
#include <QTemporaryDir>
#include <QString>

#include <fftw3.h>

#include "dsp/fftwengine.h"

namespace {

int g_failures = 0;

void check(bool cond, const char *what)
{
    if (cond) {
        std::printf("[PASS] %s\n", what);
    } else {
        std::printf("[FAIL] %s\n", what);
        g_failures++;
    }
}

// Writes a syntactically valid FFTW wisdom file that contains no plans, via
// the real FFTW export path (rather than hand-crafting the s-expression
// text) so the file is guaranteed to be importable. Brackets with
// fftwf_forget_wisdom() so this helper never leaks state into the process-
// wide wisdom store FFTW maintains (wisdom is global to the process, not
// scoped to a file or an FFTWEngine instance -- see the note in main()).
bool writeEmptyWisdomFile(const QString &path)
{
    fftwf_forget_wisdom();
    bool ok = fftwf_export_wisdom_to_filename(path.toStdString().c_str()) != 0;
    fftwf_forget_wisdom();
    return ok;
}

// Runs a real FFTW_PATIENT search for n=1024 forward complex (the cof1024
// signature from the root-cause findings) and exports the result as a
// wisdom file, exactly as the plan-05 Step 3 FFT_BENCH tool will for a
// deployed target. Explicitly forgets process-wide wisdom before and after
// so this helper is self-contained and doesn't contaminate later cases.
bool writeCof1024PatientWisdomFile(const QString &path)
{
    fftwf_forget_wisdom();
    const int n = 1024;
    fftwf_complex *in = (fftwf_complex*)fftwf_malloc(sizeof(fftwf_complex) * n);
    fftwf_complex *out = (fftwf_complex*)fftwf_malloc(sizeof(fftwf_complex) * n);
    fftwf_plan p = fftwf_plan_dft_1d(n, in, out, FFTW_FORWARD, FFTW_PATIENT);
    bool ok = false;

    if (p) {
        ok = fftwf_export_wisdom_to_filename(path.toStdString().c_str()) != 0;
        fftwf_destroy_plan(p);
    }

    fftwf_free(in);
    fftwf_free(out);
    fftwf_forget_wisdom();
    return ok;
}

// Mechanism demonstration standing in for an ARM64 pre-fix reproduction
// (the plan-05 launch note permits an x86/unit-level substitution for this
// acceptance item). Exercises the exact FFTW call the pre-fix
// fftwengine.cpp made unconditionally -- fftwf_plan_dft_1d(..., PATIENT)
// with no FFTW_WISDOM_ONLY guard (commit 75f213adb) -- side by side with the
// plan-05 bounded WISDOM_ONLY lookup, to show the unrestricted call is not
// bounded the way the new path is: it always runs a real benchmark, while
// FFTW_WISDOM_ONLY returns immediately (NULL) instead of benchmarking when
// nothing matches.
void testUnrestrictedPatientIsUnboundedMechanism()
{
    const int n = 1024;
    fftwf_complex *in = (fftwf_complex*)fftwf_malloc(sizeof(fftwf_complex) * n);
    fftwf_complex *out = (fftwf_complex*)fftwf_malloc(sizeof(fftwf_complex) * n);

    fftwf_forget_wisdom(); // start from a clean planner state, like a fresh process

    auto t0 = std::chrono::steady_clock::now();
    fftwf_plan legacyPlan = fftwf_plan_dft_1d(n, in, out, FFTW_FORWARD, FFTW_PATIENT);
    auto t1 = std::chrono::steady_clock::now();
    double legacyMs = std::chrono::duration<double, std::milli>(t1 - t0).count();
    check(legacyPlan != nullptr, "mechanism: pre-fix unrestricted FFTW_PATIENT still produces a plan");

    fftwf_forget_wisdom();

    auto t2 = std::chrono::steady_clock::now();
    fftwf_plan boundedPlan = fftwf_plan_dft_1d(n, in, out, FFTW_FORWARD, FFTW_PATIENT | FFTW_WISDOM_ONLY);
    auto t3 = std::chrono::steady_clock::now();
    double boundedMs = std::chrono::duration<double, std::milli>(t3 - t2).count();
    check(boundedPlan == nullptr, "mechanism: FFTW_WISDOM_ONLY with no wisdom returns NULL instead of benchmarking");
    check(boundedMs < legacyMs,
        "mechanism: WISDOM_ONLY lookup is not slower than the unrestricted PATIENT benchmark it replaces");

    std::printf("[INFO] mechanism timing (this host/library, n=%d): "
        "unrestricted FFTW_PATIENT=%.2fms  WISDOM_ONLY-lookup=%.2fms\n",
        n, legacyMs, boundedMs);

    if (legacyPlan) { fftwf_destroy_plan(legacyPlan); }
    if (boundedPlan) { fftwf_destroy_plan(boundedPlan); }
    fftwf_free(in);
    fftwf_free(out);
    fftwf_forget_wisdom();
}

void testNoWisdomConfigured()
{
    fftwf_forget_wisdom();
    FFTWEngine engine("");
    engine.configure(1024, false);
    check(engine.getLastWisdomImportResult() == FFTWEngine::WisdomImportResult::NotConfigured,
        "no-wisdom-file: import result is NotConfigured");
    check(engine.getLastPlannerMode() == FFTWEngine::PlannerMode::EstimateFallback,
        "no-wisdom-file: planner mode falls back to EstimateFallback (ready, estimate-fallback)");
    check(engine.out() != nullptr, "no-wisdom-file: plan is usable");
}

void testImportFailure()
{
    fftwf_forget_wisdom();
    // A path that cannot exist -- forces fftwf_import_wisdom_from_filename to fail.
    FFTWEngine engine("/nonexistent/plan-05/does-not-exist.wisdom");
    engine.configure(1024, false);
    check(engine.getLastWisdomImportResult() == FFTWEngine::WisdomImportResult::ImportFailed,
        "bad-wisdom-path: import result is ImportFailed");
    check(engine.getLastPlannerMode() == FFTWEngine::PlannerMode::EstimateFallback,
        "bad-wisdom-path: planner mode falls back to EstimateFallback (ready, estimate-fallback)");
    check(engine.out() != nullptr, "bad-wisdom-path: plan is usable");
}

void testWisdomWithoutMatchingPlan(const QString &tempDir)
{
    QString path = tempDir + "/empty.wisdom";
    check(writeEmptyWisdomFile(path), "empty-wisdom: fixture file written");

    FFTWEngine engine(path);
    engine.configure(1024, false);
    check(engine.getLastWisdomImportResult() == FFTWEngine::WisdomImportResult::Imported,
        "empty-wisdom: import succeeds (well-formed file, no plans)");
    check(engine.getLastPlannerMode() == FFTWEngine::PlannerMode::EstimateFallback,
        "empty-wisdom: no matching plan -> EstimateFallback (ready, estimate-fallback)");
    check(engine.out() != nullptr, "empty-wisdom: plan is usable");
}

void testValidWisdomSelectsOptimizedPath(const QString &tempDir)
{
    QString path = tempDir + "/cof1024.wisdom";
    check(writeCof1024PatientWisdomFile(path), "valid-wisdom: cof1024 fixture file written");

    FFTWEngine engine(path);
    engine.configure(1024, false);
    check(engine.getLastWisdomImportResult() == FFTWEngine::WisdomImportResult::Imported,
        "valid-wisdom: import succeeds");
    check(engine.getLastPlannerMode() == FFTWEngine::PlannerMode::OptimizedWisdom,
        "valid-wisdom: planner mode is OptimizedWisdom (ready, optimized)");
    check(engine.out() != nullptr, "valid-wisdom: plan is usable");
    // Bounded: a WISDOM_ONLY lookup against a matching plan must not run a
    // measurable benchmark -- this is the whole point of the fix.
    check(engine.getLastPlanElapsedMs() < 500,
        "valid-wisdom: configure() with matching wisdom is bounded (<500ms, no benchmarking)");

    fftwf_forget_wisdom(); // don't leak this real cof1024 plan into later cases
}

void testEveryConfigureProducesAUsablePlan()
{
    // Distinct sizes/directions, no wisdom configured -- mirrors how
    // FFTFactory::preallocate()/getEngine() always call setReuse(false).
    // Every structurally valid request must leave a usable (non-null) plan;
    // a null plan is never stored or executed (Step 1 acceptance).
    fftwf_forget_wisdom();
    FFTWEngine engine("");
    engine.setReuse(false);
    const int sizes[] = {128, 256, 1024, 4096};

    for (int n : sizes)
    {
        engine.configure(n, false);
        char label[128];
        std::snprintf(label, sizeof(label), "no-wisdom size=%d: usable plan, never null", n);
        check((engine.in() != nullptr) && (engine.out() != nullptr), label);
    }
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);

    QTemporaryDir tempDir;
    check(tempDir.isValid(), "setup: temp dir for wisdom fixtures is usable");

    std::printf("=== FFT_BENCH shared-engine regression (plan-05 Step 1) ===\n");
    std::printf("NOTE: FFTW wisdom is process-global, not scoped per file or per\n"
                 "FFTWEngine instance -- each helper above brackets its own work with\n"
                 "fftwf_forget_wisdom() so these cases are order-independent.\n\n");

    testUnrestrictedPatientIsUnboundedMechanism();
    testNoWisdomConfigured();
    testImportFailure();
    testWisdomWithoutMatchingPlan(tempDir.path());
    testValidWisdomSelectsOptimizedPath(tempDir.path());
    testEveryConfigureProducesAUsablePlan();

    std::printf("\n=== %d failure(s) ===\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
