///////////////////////////////////////////////////////////////////////////////////
// Copyright (C) 2012 maintech GmbH, Otto-Hahn-Str. 15, 97204 Hoechberg, Germany //
// written by Christian Daniel                                                   //
// Copyright (C) 2020 Edouard Griffiths, F4EXB <f4exb06@gmail.com>               //
// Copyright (C) 2023 Jon Beniston, M7RCE <jon@beniston.com>                     //
//                                                                               //
// This program is free software; you can redistribute it and/or modify          //
// it under the terms of the GNU General Public License as published by          //
// the Free Software Foundation as version 3 of the License, or                  //
// (at your option) any later version.                                           //
//                                                                               //
// This program is distributed in the hope that it will be useful,               //
// but WITHOUT ANY WARRANTY; without even the implied warranty of                //
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the                  //
// GNU General Public License V3 for more details.                               //
//                                                                               //
// You should have received a copy of the GNU General Public License             //
// along with this program. If not, see <http://www.gnu.org/licenses/>.          //
///////////////////////////////////////////////////////////////////////////////////

#include <QDebug>
#include <QElapsedTimer>

#include "dsp/fftwengine.h"
#include "util/profiler.h"

FFTWEngine::FFTWEngine(const QString& fftWisdomFileName) :
    m_fftWisdomFileName(fftWisdomFileName),
	m_plans(),
	m_currentPlan(nullptr),
    m_reuse(true),
    m_lastWisdomImportResult(WisdomImportResult::NotConfigured),
    m_lastPlannerMode(PlannerMode::Unknown),
    m_lastPlanSize(0),
    m_lastPlanInverse(false),
    m_lastPlanElapsedMs(0)
{
}

FFTWEngine::~FFTWEngine()
{
	freeAll();
}

const QString FFTWEngine::m_name = "FFTW";

QString FFTWEngine::getName() const
{
    return m_name;
}

QString FFTWEngine::wisdomImportResultToString(WisdomImportResult result)
{
    switch (result)
    {
    case WisdomImportResult::NotConfigured: return "not-configured";
    case WisdomImportResult::ImportFailed:  return "import-failed";
    case WisdomImportResult::Imported:      return "imported";
    default:                                return "unknown";
    }
}

QString FFTWEngine::plannerModeToString(PlannerMode mode)
{
    switch (mode)
    {
    case PlannerMode::Unknown:          return "unknown";
    case PlannerMode::OptimizedWisdom:  return "optimized-wisdom";
    case PlannerMode::EstimateFallback: return "estimate-fallback";
    default:                            return "unknown";
    }
}

void FFTWEngine::configure(int n, bool inverse)
{
    if (m_reuse)
    {
        for (Plans::const_iterator it = m_plans.begin(); it != m_plans.end(); ++it)
        {
            if (((*it)->n == n) && ((*it)->inverse == inverse))
            {
                m_currentPlan = *it;
                return;
            }
        }
    }

	m_currentPlan = new Plan;
	m_currentPlan->n = n;
	m_currentPlan->inverse = inverse;
	m_currentPlan->in = (fftwf_complex*)fftwf_malloc(sizeof(fftwf_complex) * n);
	m_currentPlan->out = (fftwf_complex*)fftwf_malloc(sizeof(fftwf_complex) * n);
	QElapsedTimer t;
	t.start();
    m_globalPlanMutex.lock();

    m_lastPlanSize = n;
    m_lastPlanInverse = inverse;
    int direction = inverse ? FFTW_BACKWARD : FFTW_FORWARD;
    fftwf_plan plan = nullptr;

    // FFT_BENCH runtime policy (plan-05 Step 2): normal server startup and
    // device-set creation must never run an unrestricted FFTW_PATIENT
    // benchmark -- that synchronously pins a CPU for seconds to minutes on a
    // slow target and wedges the single-threaded REST control plane (see
    // findings-2026-07-20-fft-bench-root-cause.md). FFTW_WISDOM_ONLY turns
    // FFTW_PATIENT into a bounded lookup: it returns a plan only when one is
    // already present in the imported wisdom, and returns NULL immediately
    // otherwise -- it never benchmarks candidate plans itself. Missing,
    // invalid, or incompatible wisdom falls back to FFTW_ESTIMATE, which also
    // never benchmarks. FFTW_MEASURE is deliberately never used as the
    // fallback: it still benchmarks synchronously and would reintroduce the
    // same failure class.
    if (m_fftWisdomFileName.size() > 0)
    {
        int rc = fftwf_import_wisdom_from_filename(m_fftWisdomFileName.toStdString().c_str());

        if (rc == 0)
        { // that's an error (undocumented)
            m_lastWisdomImportResult = WisdomImportResult::ImportFailed;
            qWarning("FFTWEngine::configure: importing from FFTW wisdom file failed: '%s' -- ESTIMATE fallback",
                qPrintable(m_fftWisdomFileName));
        }
        else
        {
            m_lastWisdomImportResult = WisdomImportResult::Imported;
            qDebug("FFTWEngine::configure: successfully imported from FFTW wisdom file: '%s'", qPrintable(m_fftWisdomFileName));

            plan = fftwf_plan_dft_1d(n, m_currentPlan->in, m_currentPlan->out, direction, FFTW_PATIENT | FFTW_WISDOM_ONLY);

            if (!plan)
            {
                qWarning("FFTWEngine::configure: no matching FFT_BENCH wisdom plan for n=%d,%s in '%s' -- ESTIMATE fallback",
                    n, inverse ? "inverse" : "forward", qPrintable(m_fftWisdomFileName));
            }
        }
    }
    else
    {
        m_lastWisdomImportResult = WisdomImportResult::NotConfigured;
        qWarning("FFTWEngine::configure: no FFTW wisdom file configured for n=%d,%s -- ESTIMATE fallback",
            n, inverse ? "inverse" : "forward");
    }

    if (plan)
    {
        m_lastPlannerMode = PlannerMode::OptimizedWisdom;
    }
    else
    {
        plan = fftwf_plan_dft_1d(n, m_currentPlan->in, m_currentPlan->out, direction, FFTW_ESTIMATE);
        m_lastPlannerMode = plan ? PlannerMode::EstimateFallback : PlannerMode::Unknown;
    }

    m_globalPlanMutex.unlock();

    m_lastPlanElapsedMs = t.elapsed();

    if (!plan)
    {
        // A null FFTW plan must never be stored or executed (plan-05 Step 1
        // acceptance). This should not happen in practice -- FFTW_ESTIMATE
        // always succeeds for a structurally valid size -- but guard it
        // explicitly rather than push a broken Plan onto m_plans.
        qCritical("FFTWEngine::configure: fftwf_plan_dft_1d returned NULL for n=%d,%s even with FFTW_ESTIMATE -- discarding",
            n, inverse ? "inverse" : "forward");
        fftwf_free(m_currentPlan->in);
        fftwf_free(m_currentPlan->out);
        delete m_currentPlan;
        m_currentPlan = nullptr;
        return;
    }

    m_currentPlan->plan = plan;

    if (m_lastPlannerMode == PlannerMode::OptimizedWisdom)
    {
        qInfo("FFTWEngine::configure: n=%d,%s source=FFT_BENCH wisdom ('%s') planner=%s took %lld ms",
            n, inverse ? "inverse" : "forward", qPrintable(m_fftWisdomFileName),
            qPrintable(plannerModeToString(m_lastPlannerMode)), m_lastPlanElapsedMs);
    }
    else
    {
        qWarning("FFTWEngine::configure: n=%d,%s wisdom=%s planner=%s (ESTIMATE fallback, degraded performance) took %lld ms",
            n, inverse ? "inverse" : "forward",
            qPrintable(wisdomImportResultToString(m_lastWisdomImportResult)),
            qPrintable(plannerModeToString(m_lastPlannerMode)), m_lastPlanElapsedMs);
    }

	m_plans.push_back(m_currentPlan);
}

void FFTWEngine::transform()
{
    PROFILER_START()

	if (m_currentPlan != nullptr)
	{
		fftwf_execute(m_currentPlan->plan);
		PROFILER_STOP(QString("%1 %2").arg(getName()).arg(m_currentPlan->n))
	}
}

Complex* FFTWEngine::in()
{
	if(m_currentPlan != NULL)
		return reinterpret_cast<Complex*>(m_currentPlan->in);
	else return NULL;
}

Complex* FFTWEngine::out()
{
	if(m_currentPlan != NULL)
		return reinterpret_cast<Complex*>(m_currentPlan->out);
	else return NULL;
}

QMutex FFTWEngine::m_globalPlanMutex;

void FFTWEngine::freeAll()
{
	for(Plans::iterator it = m_plans.begin(); it != m_plans.end(); ++it) {
		fftwf_destroy_plan((*it)->plan);
		fftwf_free((*it)->in);
		fftwf_free((*it)->out);
		delete *it;
	}
	m_plans.clear();
}
