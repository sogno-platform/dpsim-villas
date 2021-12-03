/* Copyright 2017-2020 Institute for Automation of Complex Power Systems,
 *                     EONERC, RWTH Aachen University
 * DPsim
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * any later version.
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 *********************************************************************************/

#include <stdexcept>
#include <cstdio>
#include <cstdlib>

#include <spdlog/sinks/stdout_color_sinks.h>

#include <dpsim-villas/InterfaceShmem.h>
#include <cps/Logger.h>

using namespace CPS;
using namespace DPsim;

void InterfaceShmem::open(CPS::Logger::Log log) {
	mLog = log;

	mLog->info("Opening InterfaceShmem: {} <-> {}", mWName, mRName);

	if (shmem_int_open(mWName.c_str(), mRName.c_str(), &mShmem, &mConf) < 0) {
		mLog->error("Failed to open/map shared memory object");
		std::exit(1);
	}

	mLog->info("Opened InterfaceShmem: {} <-> {}", mWName, mRName);

	mSequence = 0;

	if (shmem_int_alloc(&mShmem, &mLastSample, 1) < 0) {
		mLog->info("Failed to allocate single sample from pool");
		close();
		std::exit(1);
	}

	mLastSample->sequence = 0;
	mLastSample->ts.origin.tv_sec = 0;
	mLastSample->ts.origin.tv_nsec = 0;

	std::memset(&mLastSample->data, 0, mLastSample->capacity * sizeof(float));
}

void InterfaceShmem::close() {
	shmem_int_close(&mShmem);
	InterfaceSampleBased::close();
}

void InterfaceShmem::readValues(bool blocking) {
	// mLog->info("ReadValues InterfaceShmem: blocking={}", blocking);
	Sample *sample = nullptr;
	int ret = 0;
	try {
		if (!blocking) {
			// mLog->info("Check if theres actually data available.");
			ret = queue_signalled_available(&mShmem.read.shared->queue);
			if (ret <= 0) {
				// mLog->info("No data available from queue_signalled_available.");
				return;
			}
				

			ret = shmem_int_read(&mShmem, &sample, 1);
			if (ret == 0) {
				// mLog->info("No data available from shmem_int_read.");
				return;
			}
		}
		else {
			// mLog->info("Enter while-loop for shmem_int_read");
			while (ret == 0)
				ret = shmem_int_read(&mShmem, &sample, 1);
			// mLog->info("Leave while-loop for shmem_int_read");
		}
		if (ret < 0) {
			mLog->error("Fatal error: failed to read sample from InterfaceShmem");
			close();
			std::exit(1);
		}

		// mLog->info("Setting import attributes according to sample.");
		for (auto imp : mImports) {
			imp(sample);
		}

		sample_decref(sample);
	}
	catch (std::exception& exc) {
		/* probably won't happen (if the timer expires while we're still reading data,
		 * we have a bigger problem somewhere else), but nevertheless, make sure that
		 * we're not leaking memory from the queue pool */
		if (sample)
			sample_decref(sample);

		throw exc;
	}
}

void InterfaceShmem::writeValues() {
	Sample *sample = nullptr;
	Int ret = 0;
	bool done = false;
	try {
		if (shmem_int_alloc(&mShmem, &sample, 1) < 1) {
			mLog->error("Fatal error: pool underrun in: {} <-> {} at sequence no {}", mWName, mRName, mSequence);
			close();
			std::exit(1);
		}

		for (auto exp : mExports) {
			exp(sample);
		}

		sample->sequence = mSequence++;
		sample->flags |= (int) villas::node::SampleFlags::HAS_DATA;
		clock_gettime(CLOCK_REALTIME, &sample->ts.origin);
		done = true;

		do {
			ret = shmem_int_write(&mShmem, &sample, 1);
		} while (ret == 0);
		if (ret < 0)
			mLog->error("Failed to write samples to InterfaceShmem");

		sample_copy(mLastSample, sample);
	}
	catch (std::exception& exc) {
		/* We need to at least send something, so determine where exactly the
		 * timer expired and either resend the last successfully sent sample or
		 * just try to send this one again.
		 * TODO: can this be handled better? */
		if (!done)
			sample = mLastSample;

		while (ret == 0)
			ret = shmem_int_write(&mShmem, &sample, 1);

		if (ret < 0)
			mLog->error("Failed to write samples to InterfaceShmem");

		/* Don't throw here, because we managed to send something */
	}
}

void InterfaceShmem::PreStep::execute(Real time, Int timeStepCount) {
	if (timeStepCount % mIntf.mDownsampling == 0)
		mIntf.readValues(mIntf.mSync);
}

void InterfaceShmem::PostStep::execute(Real time, Int timeStepCount) {
	if (timeStepCount % mIntf.mDownsampling == 0)
		mIntf.writeValues();
}

Attribute<Int>::Ptr InterfaceShmem::importInt(UInt idx) {
	Attribute<Int>::Ptr attr = Attribute<Int>::make(Flags::read | Flags::write);
	auto& log = mLog;
	addImport([attr, idx, log](Sample *smp) {
		if (idx >= smp->length) {
			log->error("incomplete data received from InterfaceShmem");
			return;
		}
		attr->set(smp->data[idx].i);
	});
	mImportAttrs.push_back(attr);
	return attr;
}

Attribute<Real>::Ptr InterfaceShmem::importReal(UInt idx) {
	Attribute<Real>::Ptr attr = Attribute<Real>::make(Flags::read | Flags::write);
	auto& log = mLog;
	addImport([attr, idx, log](Sample *smp) {
		if (idx >= smp->length) {
			log->error("incomplete data received from InterfaceShmem");
			return;
		}
		attr->set(smp->data[idx].f);
	});
	mImportAttrs.push_back(attr);
	return attr;
}

Attribute<Bool>::Ptr InterfaceShmem::importBool(UInt idx) {
	Attribute<Bool>::Ptr attr = Attribute<Bool>::make(Flags::read | Flags::write);
	auto& log = mLog;
	addImport([attr, idx, log](Sample *smp) {
		if (idx >= smp->length) {
			log->error("incomplete data received from InterfaceShmem");
			return;
		}
		attr->set((Bool)smp->data[idx].f);
	});
	mImportAttrs.push_back(attr);
	return attr;
}

Attribute<Complex>::Ptr InterfaceShmem::importComplex(UInt idx) {
	Attribute<Complex>::Ptr attr = Attribute<Complex>::make(Flags::read | Flags::write);
	auto& log = mLog;
	addImport([attr, idx, log](Sample *smp) {
		if (idx >= smp->length) {
			log->error("incomplete data received from InterfaceShmem");
			return;
		}
		auto *z = reinterpret_cast<float*>(&smp->data[idx].z);
		auto  y = Complex(z[0], z[1]);

		attr->set(y);
	});
	mImportAttrs.push_back(attr);
	return attr;
}

Attribute<Complex>::Ptr InterfaceShmem::importComplexMagPhase(UInt idx) {
	Attribute<Complex>::Ptr attr = Attribute<Complex>::make(Flags::read | Flags::write);
	auto& log = mLog;
	addImport([attr, idx, log](Sample *smp) {
		if (idx >= smp->length) {
			log->error("incomplete data received from InterfaceShmem");
			return;
		}
		auto *z = reinterpret_cast<float*>(&smp->data[idx].z);
		auto  y = std::polar(z[0], z[1]);

		attr->set(y);
	});
	mImportAttrs.push_back(attr);
	return attr;
}

void InterfaceShmem::exportInt(Attribute<Int>::Ptr attr, UInt idx, const std::string &name, const std::string &unit) {
	addExport([attr, idx](Sample *smp) {
		if (idx >= smp->capacity)
			throw std::out_of_range("not enough space in allocated sample");
		if (idx >= smp->length)
			smp->length = idx + 1;

		smp->data[idx].i = attr->getByValue();
	});
	mExportAttrs.push_back(attr);
	mExportSignals[idx] = Signal(idx, SignalType::INTEGER, name, unit);
}

void InterfaceShmem::exportReal(Attribute<Real>::Ptr attr, UInt idx, const std::string &name, const std::string &unit) {
	addExport([attr, idx](Sample *smp) {
		if (idx >= smp->capacity)
			throw std::out_of_range("not enough space in allocated sample");
		if (idx >= smp->length)
			smp->length = idx + 1;

		smp->data[idx].f = attr->getByValue();
	});
	mExportAttrs.push_back(attr);
	mExportSignals[idx] = Signal(idx, SignalType::FLOAT, name, unit);
}

void InterfaceShmem::exportBool(Attribute<Bool>::Ptr attr, UInt idx, const std::string &name, const std::string &unit) {
	addExport([attr, idx](Sample *smp) {
		if (idx >= smp->capacity)
			throw std::out_of_range("not enough space in allocated sample");
		if (idx >= smp->length)
			smp->length = idx + 1;
		smp->data[idx].f = (Real)attr->getByValue();
	});
	mExportAttrs.push_back(attr);
	mExportSignals[idx] = Signal(idx, SignalType::BOOLEAN, name, unit);
}

void InterfaceShmem::exportComplex(Attribute<Complex>::Ptr attr, UInt idx, const std::string &name, const std::string &unit) {
	addExport([attr, idx](Sample *smp) {
		if (idx >= smp->capacity)
			throw std::out_of_range("not enough space in allocated sample");
		if (idx >= smp->length)
			smp->length = idx + 1;

		auto  y = attr->getByValue();
		auto *z = reinterpret_cast<float*>(&smp->data[idx].z);

		z[0] = y.real();
		z[1] = y.imag();
	});
	mExportAttrs.push_back(attr);
	mExportSignals[idx] = Signal(idx, SignalType::COMPLEX, name, unit);
}

Task::List InterfaceShmem::getTasks() {
	return Task::List({
		std::make_shared<InterfaceShmem::PreStep>(*this),
		std::make_shared<InterfaceShmem::PostStep>(*this)
	});
}
