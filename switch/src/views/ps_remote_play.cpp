#include "views/ps_remote_play.h"
PSRemotePlay::PSRemotePlay(Host *host)
	: host(host)
{
	this->io = IO::GetInstance();

	// Application::frame() draws whatever Application::currentFocus is every
	// frame, regardless of which Activity is actually on top - pushing this
	// Activity doesn't move focus off the game-row DetailCell that was
	// clicked to start the stream, so its highlight glow kept drawing over
	// the video every frame. Take focus for this view (it doesn't need
	// directional navigation, just needs to not be the game-row DetailCell)
	// and suppress its own highlight so nothing draws in its place either.
	this->setHideHighlight(true);
	brls::Application::giveFocus(this);
}

void PSRemotePlay::draw(NVGcontext *vg, float x, float y, float width, float height, brls::Style style, brls::FrameContext *ctx)
{
	this->io->MainLoop();
	this->host->SendFeedbackState();

	// ZL+ZR+Plus was held (see IO::UpdateControllerState) - tear the stream
	// down the same way a server-side quit does (see StartCloudStream's
	// SetEventQuitCallback in cloud_game_list.cpp), just user-triggered
	// instead of session-triggered. Do this last and return immediately:
	// popView() may destroy this view, so nothing here should touch `this`
	// afterward.
	if(this->io->exit_stream_requested.exchange(false))
	{
		// Stop and join before removing the view. popView() may immediately
		// destroy `this`, so it must be the final operation in this block.
		this->host->StopSession();
		this->host->FiniSession();
		// StopSession may deliver one final quit callback while FiniSession is
		// joining the worker. Do not carry that request into the next stream.
		this->io->exit_stream_requested.store(false);
		this->io->FreeVideo();
		brls::Application::unblockInputs();
		brls::Application::setLimitedFPS(60);
		brls::Application::notify("Stream closed");
		brls::Application::popActivity();
		return;
	}
}

PSRemotePlay::~PSRemotePlay()
{
}
