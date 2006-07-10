// qtractorAudioEngine.cpp
//
/****************************************************************************
   Copyright (C) 2005, rncbc aka Rui Nuno Capela. All rights reserved.

   This program is free software; you can redistribute it and/or
   modify it under the terms of the GNU General Public License
   as published by the Free Software Foundation; either version 2
   of the License, or (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.

*****************************************************************************/

#include "qtractorAbout.h"
#include "qtractorAudioEngine.h"
#include "qtractorAudioMonitor.h"
#include "qtractorAudioBuffer.h"

#include "qtractorMonitor.h"
#include "qtractorSessionCursor.h"
#include "qtractorSessionDocument.h"
#include "qtractorMidiEngine.h"

#include "qtractorClip.h"

#include <qapplication.h>


//----------------------------------------------------------------------
// qtractorAudioEngine_process -- JACK client process callback.
//

static int qtractorAudioEngine_process ( jack_nframes_t nframes, void *pvArg )
{
	qtractorAudioEngine *pAudioEngine
		= static_cast<qtractorAudioEngine *> (pvArg);

	return pAudioEngine->process(nframes);
}


//----------------------------------------------------------------------
// qtractorAudioEngine_timebase -- JACK timebase master callback.
//

static void qtractorAudioEngine_timebase ( jack_transport_state_t,
	jack_nframes_t, jack_position_t *pPos, int, void *pvArg )
{
	qtractorAudioEngine *pAudioEngine
		= static_cast<qtractorAudioEngine *> (pvArg);

	qtractorSession *pSession = pAudioEngine->session();
	if (pSession) {
		unsigned short iTicksPerBeat = pSession->ticksPerBeat();
		unsigned short iBeatsPerBar  = pSession->beatsPerBar();
		unsigned int   bars  = 0;
		unsigned int   beats = 0;
		unsigned long  ticks = pSession->tickFromFrame(pPos->frame);
		if (ticks >= (unsigned long) iTicksPerBeat) {
			beats  = (unsigned int)  (ticks / iTicksPerBeat);
			ticks -= (unsigned long) (beats * iTicksPerBeat);
		}
		if (beats >= (unsigned int) iBeatsPerBar) {
			bars   = (unsigned int) (beats / iBeatsPerBar);
			beats -= (unsigned int) (bars  * iBeatsPerBar);
		}
		// Time frame code in bars.beats.ticks ...
		pPos->valid = JackPositionBBT;
		pPos->bar   = bars  + 1;
		pPos->beat  = beats + 1;
		pPos->tick  = ticks;
		// Keep current tempo (BPM)...
		pPos->beats_per_bar    = iBeatsPerBar;
		pPos->ticks_per_beat   = iTicksPerBeat;
		pPos->beats_per_minute = pSession->tempo();
	//	pPos->beat_type        = 4.0;	// Quarter note.
	}
}


//----------------------------------------------------------------------
// qtractorAudioEngine_shutdown -- JACK client shutdown callback.
//

static void qtractorAudioEngine_shutdown ( void *pvArg )
{
	qtractorAudioEngine *pAudioEngine
		= static_cast<qtractorAudioEngine *> (pvArg);

	if (pAudioEngine->notifyWidget()) {
		QApplication::postEvent(pAudioEngine->notifyWidget(),
			new QCustomEvent(pAudioEngine->notifyShutdownType(), pAudioEngine));
	}
}


//----------------------------------------------------------------------
// qtractorAudioEngine_xrun -- JACK client XRUN callback.
//

static int qtractorAudioEngine_xrun ( void *pvArg )
{
	qtractorAudioEngine *pAudioEngine
		= static_cast<qtractorAudioEngine *> (pvArg);

	if (pAudioEngine->notifyWidget()) {
		QApplication::postEvent(pAudioEngine->notifyWidget(),
			new QCustomEvent(pAudioEngine->notifyXrunType(), pAudioEngine));
	}

	return 0;
}


//----------------------------------------------------------------------
// qtractorAudioEngine_graph_order -- JACK graph change callback.
//

static int qtractorAudioEngine_graph_order ( void *pvArg )
{
	qtractorAudioEngine *pAudioEngine
		= static_cast<qtractorAudioEngine *> (pvArg);

	if (pAudioEngine->notifyWidget()) {
		QApplication::postEvent(pAudioEngine->notifyWidget(),
			new QCustomEvent(pAudioEngine->notifyPortType(), pAudioEngine));
	}

	return 0;
}


//----------------------------------------------------------------------
// qtractorAudioEngine_graph_port -- JACK port registration callback.
//

static void qtractorAudioEngine_graph_port ( jack_port_id_t, int, void *pvArg )
{
	qtractorAudioEngine *pAudioEngine
		= static_cast<qtractorAudioEngine *> (pvArg);

	if (pAudioEngine->notifyWidget()) {
		QApplication::postEvent(pAudioEngine->notifyWidget(),
			new QCustomEvent(pAudioEngine->notifyPortType(), pAudioEngine));
	}
}


//----------------------------------------------------------------------
// qtractorAudioEngine_buffer_size -- JACK buffer-size change callback.
//

static int qtractorAudioEngine_buffer_size ( jack_nframes_t nframes, void *pvArg )
{
	qtractorAudioEngine *pAudioEngine
		= static_cast<qtractorAudioEngine *> (pvArg);

	if (pAudioEngine->notifyWidget()) {
		QApplication::postEvent(pAudioEngine->notifyWidget(),
			new QCustomEvent(pAudioEngine->notifyBufferType(), pAudioEngine));
	}

	return 0;
}


//----------------------------------------------------------------------
// class qtractorAudioEngine -- JACK client instance (singleton).
//

// Constructor.
qtractorAudioEngine::qtractorAudioEngine ( qtractorSession *pSession )
	: qtractorEngine(pSession, qtractorTrack::Audio)
{
	m_pJackClient = NULL;

	m_pNotifyWidget       = NULL;
	m_eNotifyShutdownType = QEvent::None;
	m_eNotifyXrunType     = QEvent::None;
	m_eNotifyPortType     = QEvent::None;
	m_eNotifyBufferType   = QEvent::None;
}


// JACK client descriptor accessor.
jack_client_t *qtractorAudioEngine::jackClient (void) const
{
	return m_pJackClient;
}


// Event notifier widget settings.
void qtractorAudioEngine::setNotifyWidget ( QWidget *pNotifyWidget )
{
	m_pNotifyWidget = pNotifyWidget;
}

void qtractorAudioEngine::setNotifyShutdownType ( QEvent::Type eNotifyShutdownType )
{
	m_eNotifyShutdownType = eNotifyShutdownType;
}

void qtractorAudioEngine::setNotifyXrunType ( QEvent::Type eNotifyXrunType )
{
	m_eNotifyXrunType = eNotifyXrunType;
}

void qtractorAudioEngine::setNotifyPortType ( QEvent::Type eNotifyPortType )
{
	m_eNotifyPortType = eNotifyPortType;
}

void qtractorAudioEngine::setNotifyBufferType ( QEvent::Type eNotifyBufferType )
{
	m_eNotifyBufferType = eNotifyBufferType;
}


QWidget *qtractorAudioEngine::notifyWidget (void) const
{
	return m_pNotifyWidget;
}

QEvent::Type qtractorAudioEngine::notifyShutdownType (void) const
{
	return m_eNotifyShutdownType;
}

QEvent::Type qtractorAudioEngine::notifyXrunType (void) const
{
	return m_eNotifyXrunType;
}

QEvent::Type qtractorAudioEngine::notifyPortType (void) const
{
	return m_eNotifyPortType;
}

QEvent::Type qtractorAudioEngine::notifyBufferType (void) const
{
	return m_eNotifyBufferType;
}


// Internal sample-rate accessor.
unsigned int qtractorAudioEngine::sampleRate (void) const
{
	return (m_pJackClient ? jack_get_sample_rate(m_pJackClient) : 0);
}

// Buffer size accessor.
unsigned int qtractorAudioEngine::bufferSize (void) const
{
	return (m_pJackClient ? jack_get_buffer_size(m_pJackClient) : 0);
}


// Device engine initialization method.
bool qtractorAudioEngine::init ( const QString& sClientName )
{
	// Try open a new client...
	m_pJackClient = jack_client_new(sClientName.latin1());
	if (m_pJackClient == NULL)
		return false;

	// ATTN: First thing to remember to set session sample rate.
	session()->setSampleRate(sampleRate());

	return true;
}


// Device engine activation method.
bool qtractorAudioEngine::activate (void)
{
	// Set our main engine processor callbacks.
	jack_set_process_callback(m_pJackClient,
			qtractorAudioEngine_process, this);

	// Trnsport timebase callbacks...
	jack_set_timebase_callback(m_pJackClient, 0 /* FIXME: un-conditional! */,
		qtractorAudioEngine_timebase, this);

	// And some other event callbacks...
	jack_set_xrun_callback(m_pJackClient,
		qtractorAudioEngine_xrun, this);
	jack_on_shutdown(m_pJackClient,
		qtractorAudioEngine_shutdown, this);
	jack_set_graph_order_callback(m_pJackClient,
		qtractorAudioEngine_graph_order, this);
	jack_set_port_registration_callback(m_pJackClient,
		qtractorAudioEngine_graph_port, this);
    jack_set_buffer_size_callback(m_pJackClient,
		qtractorAudioEngine_buffer_size, this);

	// Time to activate ourselves...
	jack_activate(m_pJackClient);

	// Now, do all auto-connection stuff (if applicable...)
	for (qtractorBus *pBus = busses().first();
			pBus; pBus = pBus->next()) {
		qtractorAudioBus *pAudioBus
			= static_cast<qtractorAudioBus *>(pBus);
		if (pAudioBus)
			pAudioBus->autoConnect();
	}

	// We're now ready and running...
	return true;
}


// Device engine start method.
bool qtractorAudioEngine::start (void)
{
	if (!isActivated())
	    return false;

	// Start trnsport rolling...
	jack_transport_start(m_pJackClient);

	// We're now ready and running...
	return true;
}


// Device engine stop method.
void qtractorAudioEngine::stop (void)
{
	if (!isActivated())
	    return;

	jack_transport_stop(m_pJackClient);
}


// Device engine deactivation method.
void qtractorAudioEngine::deactivate (void)
{
	// We're stopping now...
	// setPlaying(false);

	// Deactivate the JACK client first.
	if (m_pJackClient)
		jack_deactivate(m_pJackClient);
}


// Device engine cleanup method.
void qtractorAudioEngine::clean (void)
{
	// Close the JACK client, finally.
	if (m_pJackClient) {
		jack_client_close(m_pJackClient);
		m_pJackClient = NULL;
	}
}


// Process cycle executive.
int qtractorAudioEngine::process ( unsigned int nframes )
{
	// Don't bother with a thing, if not running.
	if (!isActivated())
		return 1;

	// Prepare current audio busses...
	for (qtractorBus *pBus = busses().first();
		pBus; pBus = pBus->next()) {
		qtractorAudioBus *pAudioBus
			= static_cast<qtractorAudioBus *> (pBus);
		if (pAudioBus)
			pAudioBus->process_prepare(nframes);
	}

	// Don't go any further, if not playing.
	if (!isPlaying())
		return process_idle(nframes);

	// Make sure we have an actual session cursor...
	qtractorSession *pSession = session();
	if (pSession == NULL)
		return 0;
	qtractorSessionCursor *pAudioCursor = sessionCursor();
	if (pAudioCursor == NULL)
	    return 0;

	// This the legal process cycle frame range...
	unsigned long iFrameStart = pAudioCursor->frame();
	unsigned long iFrameEnd   = iFrameStart + nframes;

	// Split processing, in case we're looping...
	if (pSession->isLooping() && iFrameStart < pSession->loopEnd()) {
		// Loop-length might be shorter than the buffer-period...
		while (iFrameEnd > pSession->loopEnd()) {
			// Process the remaining until end-of-loop...
			pSession->process(pAudioCursor, iFrameStart, pSession->loopEnd());
			// Reset to start-of-loop...
			iFrameStart = pSession->loopStart();
			iFrameEnd   = iFrameStart + (iFrameEnd - pSession->loopEnd());
			// Set to new transport location...
			jack_transport_locate(m_pJackClient, iFrameStart);
			pAudioCursor->seek(iFrameStart);
		}
	}

	// Regular range...
	pSession->process(pAudioCursor, iFrameStart, iFrameEnd);

	// Commit current audio busses...
	for (qtractorBus *pBus = busses().first();
		pBus; pBus = pBus->next()) {
		qtractorAudioBus *pAudioBus
			= static_cast<qtractorAudioBus *> (pBus);
		if (pAudioBus)
			pAudioBus->process_commit(nframes);
	}

	// Sync with loop boundaries (unlikely?)
	if (pSession->isLooping() && iFrameStart < pSession->loopEnd()
		&& iFrameEnd >= pSession->loopEnd()) {
		iFrameEnd = pSession->loopStart()
			+ (iFrameEnd - pSession->loopEnd());
		// Set to new transport location...
		jack_transport_locate(m_pJackClient, iFrameEnd);
	}

	// Prepare advance for next cycle...
	pAudioCursor->seek(iFrameEnd);
	pAudioCursor->process(nframes);

	// Always sync to MIDI output thread...
	pSession->midiEngine()->sync();

	// Process session stuff...
	return 1;
}


// Idle process cycle executive.
int qtractorAudioEngine::process_idle ( unsigned int nframes )
{
	// At this time, we'll get this workaround for pre-monitoring
	// those audio tracks that are currently armed for recording...
	qtractorSession *pSession = session();
	if (pSession == NULL)
		return 0;

	if (pSession->recordTracks() < 1)
		return 1;

	for (qtractorTrack *pTrack = pSession->tracks().first();
			pTrack; pTrack = pTrack->next()) {
		// Audio-buffers needs some preparation...
		if (pTrack->isRecord()
			&& pTrack->trackType() == qtractorTrack::Audio) {
			qtractorAudioBus *pAudioBus
				= static_cast<qtractorAudioBus *> (pTrack->inputBus());
			qtractorAudioMonitor *pAudioMonitor
				= static_cast<qtractorAudioMonitor *> (pTrack->monitor());
			// Pre-monitoring...
			if (pAudioBus && pAudioMonitor && pTrack->isRecord()) {
				pAudioMonitor->process(
					pAudioBus->in(), nframes, pAudioBus->channels());
			}
		}
	}

	return 1;
}


// Document element methods.
bool qtractorAudioEngine::loadElement ( qtractorSessionDocument *pDocument,
	QDomElement *pElement )
{
	qtractorEngine::clear();

	// Load session children...
	for (QDomNode nChild = pElement->firstChild();
			!nChild.isNull();
				nChild = nChild.nextSibling()) {

		// Convert node to element...
		QDomElement eChild = nChild.toElement();
		if (eChild.isNull())
			continue;

		if (eChild.tagName() == "audio-bus") {
			QString sBusName = eChild.attribute("name");
			qtractorBus::BusMode busMode
				= pDocument->loadBusMode(eChild.attribute("mode"));
			qtractorAudioBus *pAudioBus
				= new qtractorAudioBus(this, sBusName, busMode);
			for (QDomNode nProp = eChild.firstChild();
					!nProp.isNull();
						nProp = nProp.nextSibling()) {
				// Convert audio-bus property to element...
				QDomElement eProp = nProp.toElement();
				if (eProp.isNull())
					continue;
				if (eProp.tagName() == "channels") {
					pAudioBus->setChannels(eProp.text().toUShort());
				} else if (eProp.tagName() == "auto-connect") {
					pAudioBus->setAutoConnect(
						pDocument->boolFromText(eProp.text()));
				} else if (eProp.tagName() == "input-gain") {
					if (pAudioBus->monitor_in())
						pAudioBus->monitor_in()->setGain(
							eProp.text().toFloat());
				} else if (eProp.tagName() == "input-panning") {
					if (pAudioBus->monitor_in())
						pAudioBus->monitor_in()->setPanning(
							eProp.text().toFloat());
				} else if (eProp.tagName() == "input-connects") {
					pAudioBus->loadConnects(
						pAudioBus->inputs(), pDocument, &eProp);
				} else if (eProp.tagName() == "output-gain") {
					if (pAudioBus->monitor_out())
						pAudioBus->monitor_out()->setGain(
							eProp.text().toFloat());
				} else if (eProp.tagName() == "output-panning") {
					if (pAudioBus->monitor_out())
						pAudioBus->monitor_out()->setPanning(
							eProp.text().toFloat());
				} else if (eProp.tagName() == "output-connects") {
					pAudioBus->loadConnects(
						pAudioBus->outputs(), pDocument, &eProp);
				}
			}
			qtractorEngine::addBus(pAudioBus);
		}
	}

	return true;
}


bool qtractorAudioEngine::saveElement ( qtractorSessionDocument *pDocument,
	QDomElement *pElement )
{
	// Save audio busses...
	for (qtractorBus *pBus = qtractorEngine::busses().first();
			pBus; pBus = pBus->next()) {
		qtractorAudioBus *pAudioBus
			= static_cast<qtractorAudioBus *> (pBus);
		if (pAudioBus) {
			// Create the new audio bus element...
			QDomElement eAudioBus
				= pDocument->document()->createElement("audio-bus");
			eAudioBus.setAttribute("name",
				pAudioBus->busName());
			eAudioBus.setAttribute("mode",
				pDocument->saveBusMode(pAudioBus->busMode()));
			pDocument->saveTextElement("channels",
				QString::number(pAudioBus->channels()), &eAudioBus);
			pDocument->saveTextElement("auto-connect",
				pDocument->textFromBool(pAudioBus->isAutoConnect()), &eAudioBus);
			if (pAudioBus->busMode() & qtractorBus::Input) {
				pDocument->saveTextElement("input-gain",
					QString::number(pAudioBus->monitor_in()->gain()),
						&eAudioBus);
				pDocument->saveTextElement("input-panning",
					QString::number(pAudioBus->monitor_in()->panning()),
						&eAudioBus);
				QDomElement eAudioInputs
					= pDocument->document()->createElement("input-connects");
				qtractorBus::ConnectList inputs;
				pAudioBus->updateConnects(qtractorBus::Input, inputs);
				pAudioBus->saveConnects(inputs, pDocument, &eAudioInputs);
				eAudioBus.appendChild(eAudioInputs);
			}
			if (pAudioBus->busMode() & qtractorBus::Output) {
				pDocument->saveTextElement("output-gain",
					QString::number(pAudioBus->monitor_out()->gain()),
						&eAudioBus);
				pDocument->saveTextElement("output-panning",
					QString::number(pAudioBus->monitor_out()->panning()),
						&eAudioBus);
				QDomElement eAudioOutputs
					= pDocument->document()->createElement("output-connects");
				qtractorBus::ConnectList outputs;
				pAudioBus->updateConnects(qtractorBus::Output, outputs);
				pAudioBus->saveConnects(outputs, pDocument, &eAudioOutputs);
				eAudioBus.appendChild(eAudioOutputs);
			}
			pElement->appendChild(eAudioBus);
		}
	}

	return true;
}


//----------------------------------------------------------------------
// class qtractorAudioBus -- Managed JACK port set
//

// Constructor.
qtractorAudioBus::qtractorAudioBus ( qtractorAudioEngine *pAudioEngine,
	const QString& sBusName, BusMode busMode,
	unsigned short iChannels, bool bAutoConnect )
	: qtractorBus(pAudioEngine, sBusName, busMode)
{
	m_iChannels = iChannels;

	if (busMode & qtractorBus::Input)
		m_pIAudioMonitor = new qtractorAudioMonitor(iChannels);
	else
		m_pIAudioMonitor = NULL;

	if (busMode & qtractorBus::Output)
		m_pOAudioMonitor = new qtractorAudioMonitor(iChannels);
	else
		m_pOAudioMonitor = NULL;

	m_bAutoConnect = bAutoConnect;

	m_ppIPorts  = NULL;
	m_ppOPorts  = NULL;

	m_ppIBuffer = NULL;
	m_ppOBuffer = NULL;

	m_ppXBuffer = NULL;

	m_bEnabled  = false;
}

// Destructor.
qtractorAudioBus::~qtractorAudioBus (void)
{
	close();

	if (m_pIAudioMonitor)
		delete m_pIAudioMonitor;
	if (m_pOAudioMonitor)
		delete m_pOAudioMonitor;
}


// Channel number property accessor.
void qtractorAudioBus::setChannels ( unsigned short iChannels )
{
	m_iChannels = iChannels;

	if (m_pIAudioMonitor)
		m_pIAudioMonitor->setChannels(iChannels);
	if (m_pOAudioMonitor)
		m_pOAudioMonitor->setChannels(iChannels);
}

unsigned short qtractorAudioBus::channels (void) const
{
	return m_iChannels;
}


// Auto-connection predicate.
void qtractorAudioBus::setAutoConnect ( bool bAutoConnect )
{
	m_bAutoConnect = bAutoConnect;
}

bool qtractorAudioBus::isAutoConnect (void) const
{
	return m_bAutoConnect;
}


// Register and pre-allocate bus port buffers.
bool qtractorAudioBus::open (void)
{
//	close();

	qtractorAudioEngine *pAudioEngine
		= static_cast<qtractorAudioEngine *> (engine());
	if (pAudioEngine == NULL)
		return false;
	if (pAudioEngine->jackClient() == NULL)
		return false;

	unsigned short i;

	if (busMode() & qtractorBus::Input) {
		// Register and allocate input port buffers...
		m_ppIPorts  = new jack_port_t * [m_iChannels];
		m_ppIBuffer = new float * [m_iChannels];
		const QString sIPortName(busName() + "/in_%1");
		for (i = 0; i < m_iChannels; i++) {
			m_ppIPorts[i] = jack_port_register(
				pAudioEngine->jackClient(),
				sIPortName.arg(i + 1).latin1(), JACK_DEFAULT_AUDIO_TYPE,
				JackPortIsInput, 0);
			m_ppIBuffer[i] = NULL;
		}
	}

	if (busMode() & qtractorBus::Output) {
		// Register and allocate output port buffers...
		m_ppOPorts  = new jack_port_t * [m_iChannels];
		m_ppOBuffer = new float * [m_iChannels];
		const QString sOPortName(busName() + "/out_%1");
		for (i = 0; i < m_iChannels; i++) {
			m_ppOPorts[i] = jack_port_register(
				pAudioEngine->jackClient(),
				sOPortName.arg(i + 1).latin1(), JACK_DEFAULT_AUDIO_TYPE,
				JackPortIsOutput, 0);
			m_ppOBuffer[i] = NULL;
		}
	}

	// Allocate internal working bus buffers...
	unsigned int iBufferSize = pAudioEngine->bufferSize();
	m_ppXBuffer = new float * [m_iChannels];
	for (i = 0; i < m_iChannels; i++)
		m_ppXBuffer[i] = new float [iBufferSize];

	// Finally, open for biz...
	m_bEnabled = true;

	return true;
}


// Unregister and post-free bus port buffers.
void qtractorAudioBus::close (void)
{
	// Close for biz, immediate...
	m_bEnabled = false;

	qtractorAudioEngine *pAudioEngine
		= static_cast<qtractorAudioEngine *> (engine());
	if (pAudioEngine == NULL)
		return;
	if (pAudioEngine->jackClient() == NULL)
		return;

	unsigned short i;

	if (busMode() & qtractorBus::Input) {
		// Free input ports.
		if (m_ppIPorts) {
			for (i = 0; i < m_iChannels; i++) {
				if (m_ppIPorts[i]) {
					jack_port_unregister(
						pAudioEngine->jackClient(), m_ppIPorts[i]);
					m_ppIPorts[i] = NULL;
				}
			}
			delete [] m_ppIPorts;
			m_ppIPorts = NULL;
		}
		// Free input Buffers.
		if (m_ppIBuffer)
			delete [] m_ppIBuffer;
		m_ppIBuffer = NULL;
	}

	if (busMode() & qtractorBus::Output) {
		// Free output ports.
		if (m_ppOPorts) {
			for (i = 0; i < m_iChannels; i++) {
				if (m_ppOPorts[i]) {
					jack_port_unregister(
						pAudioEngine->jackClient(), m_ppOPorts[i]);
					m_ppOPorts[i] = NULL;
				}
			}
			delete [] m_ppOPorts;
			m_ppOPorts = NULL;
		}
		// Free output Buffers.
		if (m_ppOBuffer)
			delete [] m_ppOBuffer;
		m_ppOBuffer = NULL;
	}

	// Free internal buffers.
	if (m_ppXBuffer) {
		for (i = 0; i < m_iChannels; i++)
			delete [] m_ppXBuffer[i];
		if (m_ppXBuffer)
			delete [] m_ppXBuffer;
		m_ppXBuffer = NULL;
	}
}


// Auto-connect to physical ports.
void qtractorAudioBus::autoConnect (void)
{
	qtractorAudioEngine *pAudioEngine
		= static_cast<qtractorAudioEngine *> (engine());
	if (pAudioEngine == NULL)
		return;
	if (pAudioEngine->jackClient() == NULL)
		return;

	if (!m_bAutoConnect)
		return;

	unsigned short i;

	if (busMode() & qtractorBus::Input) {
		const char **ppszOPorts
			= jack_get_ports(pAudioEngine->jackClient(), 0, 0,
				JackPortIsOutput | JackPortIsPhysical);
		if (ppszOPorts) {
			const QString sIPortName = pAudioEngine->clientName()
				+ ':' + busName() + "/in_%1";
			for (i = 0; i < m_iChannels && ppszOPorts[i]; i++) {
				jack_connect(pAudioEngine->jackClient(),
					ppszOPorts[i], sIPortName.arg(i + 1).latin1());
			}
			::free(ppszOPorts);
		}
	}

	if (busMode() & qtractorBus::Output) {
		const char **ppszIPorts
			= jack_get_ports(pAudioEngine->jackClient(), 0, 0,
				JackPortIsInput | JackPortIsPhysical);
		if (ppszIPorts) {
			const QString sOPortName = pAudioEngine->clientName()
				+ ':' + busName() + "/out_%1";
			for (i = 0; i < m_iChannels && ppszIPorts[i]; i++) {
				jack_connect(pAudioEngine->jackClient(),
					sOPortName.arg(i + 1).latin1(), ppszIPorts[i]);
			}
			::free(ppszIPorts);
		}
	}
}


// Bus mode change event.
void qtractorAudioBus::updateBusMode (void)
{
	// Have a new/old input monitor?
	if (busMode() & qtractorBus::Input) {
		if (m_pIAudioMonitor == NULL)
			m_pIAudioMonitor = new qtractorAudioMonitor(m_iChannels);
	} else if (m_pIAudioMonitor) {
		delete m_pIAudioMonitor;
		m_pIAudioMonitor = NULL;
	}

	// Have a new/old output monitor?
	if (busMode() & qtractorBus::Output) {
		if (m_pOAudioMonitor == NULL)
			m_pOAudioMonitor = new qtractorAudioMonitor(m_iChannels);
	} else if (m_pOAudioMonitor) {
		delete m_pOAudioMonitor;
		m_pOAudioMonitor = NULL;
	}
}


// Process cycle preparator.
void qtractorAudioBus::process_prepare ( unsigned int nframes )
{
	if (!m_bEnabled)
		return;

	for (unsigned short i = 0; i < m_iChannels; i++) {
		if (busMode() & qtractorBus::Input) {
			m_ppIBuffer[i] = static_cast<float *>
				(jack_port_get_buffer(m_ppIPorts[i], nframes));
		}
		if (busMode() & qtractorBus::Output) {
			m_ppOBuffer[i] = static_cast<float *>
				(jack_port_get_buffer(m_ppOPorts[i], nframes));
			::memset(m_ppOBuffer[i], 0, nframes * sizeof(float));
		}
	}

	if (m_pIAudioMonitor)
		m_pIAudioMonitor->process(m_ppIBuffer, nframes);
}


// Process cycle commitment.
void qtractorAudioBus::process_commit ( unsigned int nframes )
{
	if (!m_bEnabled)
		return;

	if (m_pOAudioMonitor)
		m_pOAudioMonitor->process(m_ppOBuffer, nframes);
}


// Bus-buffering methods.
void qtractorAudioBus::buffer_prepare ( unsigned int nframes )
{
	if (!m_bEnabled)
		return;

	for (unsigned short i = 0; i < m_iChannels; i++)
		::memset(m_ppXBuffer[i], 0, nframes * sizeof(float));
}

void qtractorAudioBus::buffer_commit ( unsigned int nframes, float fGain )
{
	if (!m_bEnabled || (busMode() & qtractorBus::Output) == 0)
		return;

	for (unsigned short i = 0; i < m_iChannels; i++) {
		for (unsigned int n = 0; n < nframes; n++)
			m_ppOBuffer[i][n] += fGain * m_ppXBuffer[i][n];
	}
}

float **qtractorAudioBus::buffer (void) const
{
	return m_ppXBuffer;
}


// Frame buffer accessors.
float **qtractorAudioBus::in (void) const
{
	return m_ppIBuffer;
}

float **qtractorAudioBus::out (void) const
{
	return m_ppOBuffer;
}


// Virtual I/O bus-monitor accessors.
qtractorMonitor *qtractorAudioBus::monitor_in (void) const
{
	return audioMonitor_in();
}

qtractorMonitor *qtractorAudioBus::monitor_out (void) const
{
	return audioMonitor_out();
}


// Audio I/O bus-monitor accessors.
qtractorAudioMonitor *qtractorAudioBus::audioMonitor_in (void) const
{
	return m_pIAudioMonitor;
}

qtractorAudioMonitor *qtractorAudioBus::audioMonitor_out (void) const
{
	return m_pOAudioMonitor;
}


// Retrive all current JACK connections for a given bus mode interface;
// return the effective number of connection attempts...
int qtractorAudioBus::updateConnects ( qtractorBus::BusMode busMode,
	ConnectList& connects, bool bConnect )
{
	qtractorAudioEngine *pAudioEngine
		= static_cast<qtractorAudioEngine *> (engine());
	if (pAudioEngine == NULL)
		return 0;

	// Modes must match, at least...
	if ((busMode & qtractorAudioBus::busMode()) == 0)
		return 0;
	if (bConnect && connects.isEmpty())
		return 0;

	// Which kind of ports?
	jack_port_t **ppPorts
		= (busMode == qtractorBus::Input ? m_ppIPorts : m_ppOPorts);
	if (ppPorts == NULL)
		return 0;

	// For each channel...
	ConnectItem item;
	for (item.index = 0; item.index < m_iChannels; item.index++) {
		// Get port connections...
		const char **ppszClientPorts = jack_port_get_all_connections(
			pAudioEngine->jackClient(), ppPorts[item.index]);
		if (ppszClientPorts) {
			// Now, for each port...
			int iClientPort = 0;
			while (ppszClientPorts[iClientPort]) {
				// Check if already in list/connected...
				const QString sClientPort = ppszClientPorts[iClientPort];
				item.clientName = sClientPort.section(':', 0, 0);
				item.portName   = sClientPort.section(':', 1, 1);
				ConnectItem *pItem = connects.find(item);
				if (pItem && bConnect)
					connects.remove(pItem);
				else if (!bConnect)
					connects.append(new ConnectItem(item));
				iClientPort++;
			}
			::free(ppszClientPorts);
		}
	}

	// Shall we proceed for actual connections?
	if (!bConnect)
		return 0;

	// Our client:port prefix template...
	QString sClientPort = pAudioEngine->clientName() + ':';
	sClientPort += busName() + '/';
	sClientPort += (busMode == qtractorBus::Input ? "in" : "out");
	sClientPort += "_%1";

	QString sOutputPort;
	QString sInputPort;

	// For each (remaining) connection, try...
	int iUpdate = 0;
	for (ConnectItem *pItem = connects.first();
			pItem; pItem = connects.next()) {
		// Mangle which is output and input...
		if (busMode == qtractorBus::Input) {
			sOutputPort = pItem->clientName + ':' + pItem->portName;
			sInputPort  = sClientPort.arg(pItem->index + 1);
		} else {
			sOutputPort = sClientPort.arg(pItem->index + 1);
			sInputPort  = pItem->clientName + ':' + pItem->portName;
		}
#ifdef CONFIG_DEBUG
		fprintf(stderr, "qtractorAudioBus::updateConnects(%p, %d): "
			"jack_connect: [%s] => [%s]\n", this, (int) busMode,
				sOutputPort.latin1(), sInputPort.latin1());
#endif
		// Do it...
		if (jack_connect(pAudioEngine->jackClient(),
				sOutputPort.latin1(), sInputPort.latin1()) == 0) {
			connects.remove(pItem);
			iUpdate++;
		}
	}
	
	// Done.
	return iUpdate;
}


// end of qtractorAudioEngine.cpp
