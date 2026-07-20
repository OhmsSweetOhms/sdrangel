///////////////////////////////////////////////////////////////////////////////////
// Copyright (C) 2012 maintech GmbH, Otto-Hahn-Str. 15, 97204 Hoechberg, Germany //
// written by Christian Daniel                                                   //
// Copyright (C) 2015-2016, 2018, 2020 Edouard Griffiths, F4EXB <f4exb06@gmail.com> //
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

#ifndef INCLUDE_FFTWENGINE_H
#define INCLUDE_FFTWENGINE_H

#include <QMutex>
#include <QString>

#include <fftw3.h>
#include <list>
#include "dsp/fftengine.h"
#include "export.h"

class SDRBASE_API FFTWEngine : public FFTEngine {
public:
    // FFT_BENCH runtime policy (plan-05): result of attempting to import the
    // configured FFTW wisdom file for a configure() call.
    enum class WisdomImportResult {
        NotConfigured,  //!< No wisdom file name was configured for this engine
        ImportFailed,   //!< A wisdom file name was configured but the import call failed
        Imported        //!< Wisdom was imported successfully (a matching plan may still be absent)
    };

    // Which planner strategy actually produced the plan in use. OptimizedWisdom
    // means FFTW_PATIENT | FFTW_WISDOM_ONLY resolved a pre-computed plan from
    // imported wisdom without benchmarking. EstimateFallback means no matching
    // wisdom plan was available and the bounded FFTW_ESTIMATE plan was used
    // instead. Unknown means plan creation failed outright (see Step 1
    // acceptance: a null plan is never stored or executed).
    enum class PlannerMode {
        Unknown,
        OptimizedWisdom,
        EstimateFallback
    };

	FFTWEngine(const QString& fftWisdomFileName);
	virtual ~FFTWEngine();

	virtual void configure(int n, bool inverse);
	virtual void transform();

	virtual Complex* in();
	virtual Complex* out();

    virtual void setReuse(bool reuse) { m_reuse = reuse; }
    QString getName() const override;
    static const QString m_name;

    // Diagnostics for the most recently completed configure() call. Used by the
    // FFT_BENCH shared-engine regression (plan-05 Step 1) and by startup
    // logging (plan-05 Step 4) to make the chosen planner mode observable.
    WisdomImportResult getLastWisdomImportResult() const { return m_lastWisdomImportResult; }
    PlannerMode getLastPlannerMode() const { return m_lastPlannerMode; }
    int getLastPlanSize() const { return m_lastPlanSize; }
    bool getLastPlanInverse() const { return m_lastPlanInverse; }
    qint64 getLastPlanElapsedMs() const { return m_lastPlanElapsedMs; }

    static QString wisdomImportResultToString(WisdomImportResult result);
    static QString plannerModeToString(PlannerMode mode);

protected:
	static QMutex m_globalPlanMutex;
    QString m_fftWisdomFileName;

	struct Plan {
		int n;
		bool inverse;
		fftwf_plan plan;
		fftwf_complex* in;
		fftwf_complex* out;
	};
	typedef std::list<Plan*> Plans;
	Plans m_plans;
	Plan* m_currentPlan;
    bool m_reuse;

    WisdomImportResult m_lastWisdomImportResult;
    PlannerMode m_lastPlannerMode;
    int m_lastPlanSize;
    bool m_lastPlanInverse;
    qint64 m_lastPlanElapsedMs;

	void freeAll();
};

#endif // INCLUDE_FFTWENGINE_H
