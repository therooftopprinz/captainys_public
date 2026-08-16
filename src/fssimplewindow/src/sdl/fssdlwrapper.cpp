/* SDL2 + OpenGL backend for fssimplewindow (KMSDRM / handheld). */
#include "../fssimplewindow.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <chrono>
#include <vector>
#include <algorithm>

#include <SDL2/SDL.h>
#ifdef YS_USE_OPENGL_ES2
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <ysglslplain2ddrawing.h>
#include <ysglslsharedrenderer.h>
#include <ysglsldrawfontbitmap.h>
#include <ysglstatecache.h>
#else
#include <GL/gl.h>
#endif
#include <ysglfontdata.h>
#include <ysglbmpblit.h>

#ifndef FSKEY_NUM_KEYCODE
#define FSKEY_NUM_KEYCODE 256
#endif

static SDL_Window *ysWin = nullptr;
static SDL_GLContext ysCtx = nullptr;
#define YS_MAX_PAD 4
static SDL_GameController *ysPads[YS_MAX_PAD];
static SDL_Cursor *ysCursor = nullptr;
static int ysWid = 640, ysHei = 480;
static int exposure = 0;
static int fsKeyPress[FSKEY_NUM_KEYCODE];
static int cursorMode = 1;
static int keyboardMode = 0;
static int keyboardLowerCase = 0;
static int keyboardX = 0, keyboardY = 0;
static int selectButtonDown = 0, selectChordUsed = 0;
static Uint32 selectDownAt = 0;
static int throttleGateDown = 0, throttleGateUsed = 0;
static Uint32 throttleGateDownAt = 0;
/* Longest press still read as a tap on the two buttons that are both a modifier
   and an action of their own.  Past this the player is holding the modifier, and
   letting go must do nothing even when the modifier went unused: neither the
   afterburner nor a change of input mode is something to spring on someone who
   reached for a chord and thought better of it. */
static const Uint32 padTapMs = 250;
static int quitRequested = 0;
/* Key each pad button pressed, so a release always clears the key the press
   started, even if the Select modifier or the input mode changed meanwhile. */
static int buttonKey[SDL_CONTROLLER_BUTTON_MAX];
static Uint32 lastControllerTick = 0;
static Uint32 modeBannerUntil = 0;
static double cursorAccumulatorX = 0.0, cursorAccumulatorY = 0.0;
static int leftTriggerDown = 0, rightTriggerDown = 0;
/* Remember what each trigger pressed.  Select may be released before the
   trigger, and the release must still clear the same key. */
static int leftTriggerKey = FSKEY_NULL, rightTriggerKey = FSKEY_NULL;

static int nKeyBufUsed = 0;
static int keyBuffer[256];
static int nCharBufUsed = 0;
static int charBuffer[256];

class FsMouseEventLog
{
public:
	int eventType, lb, mb, rb, mx, my;
};
static int nMosBufUsed = 0;
static FsMouseEventLog mosBuffer[256];
static int lastLb = 0, lastMb = 0, lastRb = 0, lastMx = 0, lastMy = 0;
static int mouseCursorVisible = 1;

static void PushKey(int fskey);
static void PushChar(int c);

static const char keyboardRows[4][12]=
{
	"ABCDEFGHIJK",
	"LMNOPQRSTUV",
	"WXYZ0123456",
	"789 -_.@/?>"
};

static void OpenGameController(int joystickIndex)
{
	if(!SDL_IsGameController(joystickIndex))
	{
		return;
	}
	for(int i=0; i<YS_MAX_PAD; ++i)
	{
		if(nullptr==ysPads[i])
		{
			ysPads[i]=SDL_GameControllerOpen(joystickIndex);
			if(nullptr!=ysPads[i])
			{
				printf("Opened game controller %d: %s\n",
				    joystickIndex,SDL_GameControllerName(ysPads[i]));
			}
			return;
		}
	}
}

static int AnyPadOpen(void)
{
	for(int i=0; i<YS_MAX_PAD; ++i)
	{
		if(nullptr!=ysPads[i])
		{
			return 1;
		}
	}
	return 0;
}

/* Largest deflection across every pad, so it does not matter which one the
   pilot picks up. */
static int PadAxis(SDL_GameControllerAxis axis)
{
	int best=0;
	for(int i=0; i<YS_MAX_PAD; ++i)
	{
		if(nullptr!=ysPads[i])
		{
			const int v=SDL_GameControllerGetAxis(ysPads[i],axis);
			if(abs(v)>abs(best))
			{
				best=v;
			}
		}
	}
	return best;
}

/* Cubic response between a slow creep and a full sweep of the screen.  The
   creep floor matters more than the top speed: without it the first part of the
   stick travel produces no motion at all and the cursor feels like it jumps. */
static double CursorSpeedFromAxis(int raw)
{
	const int dead=3200;
	const double slowest=14.0;   /* pixels per second just past the dead zone */
	const double fastest=620.0;
	if(abs(raw)<=dead)
	{
		return 0.0;
	}
	const double t=((double)abs(raw)-(double)dead)/(32767.0-(double)dead);
	const double pixelsPerSecond=slowest+t*t*t*(fastest-slowest);
	return raw<0 ? -pixelsPerSecond : pixelsPerSecond;
}

/* Throttle sits on the right stick, alongside the rudder.  It is a rate control
   rather than an absolute lever, so it is driven from here instead of from a
   joystick axis assignment: the stick only meters how often a step is applied,
   and whatever was last commanded is retained once the stick centres.  R1 must
   be held, so that a rudder correction on the same stick cannot change power by
   accident. */
static void PollThrottleStick(Uint32 now)
{
	static Uint32 nextPulse=0;

	if(!throttleGateDown)
	{
		nextPulse=0;
		return;
	}

	const int raw=PadAxis(SDL_CONTROLLER_AXIS_RIGHTY);
	const int dead=4000;
	if(abs(raw)<=dead)
	{
		nextPulse=0;  /* the next deflection takes effect at once */
		return;
	}
	if(0!=nextPulse && now<nextPulse)
	{
		return;
	}

	const double t=((double)abs(raw)-(double)dead)/(32767.0-(double)dead);
	const Uint32 period=(Uint32)(300.0-240.0*t);  /* 300 ms creep .. 60 ms sweep */
	const int fine=(t<0.35);   /* 2.5% steps near the dead zone, 5% past it */
	const int up=(raw<0);      /* SDL reports a forward push as negative */

	PushKey(up ? (fine ? FSKEY_E : FSKEY_Q) : (fine ? FSKEY_D : FSKEY_A));
	nextPulse=now+period;
	throttleGateUsed=1;  /* R1 was a throttle gate, not an afterburner tap */
}

static void QueueMouseEvent(int eventType)
{
	if(nMosBufUsed < 256)
	{
		mosBuffer[nMosBufUsed++] = {eventType,lastLb,lastMb,lastRb,lastMx,lastMy};
	}
}

static void SetCursorMode(int enable)
{
	cursorMode=(0!=enable);
	if(!cursorMode)
	{
		keyboardMode=0;
	}

	/* Anything still held belongs to the mode being left. */
	for(int i=0; i<SDL_CONTROLLER_BUTTON_MAX; ++i)
	{
		if(FSKEY_NULL!=buttonKey[i])
		{
			fsKeyPress[buttonKey[i]]=0;
			buttonKey[i]=FSKEY_NULL;
		}
	}
	fsKeyPress[FSKEY_LBRACKET]=0;
	fsKeyPress[FSKEY_RBRACKET]=0;
	throttleGateDown=0;

	mouseCursorVisible=cursorMode;
	SDL_ShowCursor(cursorMode ? SDL_ENABLE : SDL_DISABLE);
	modeBannerUntil=SDL_GetTicks()+2000;
	printf("R36S input mode: %s\n",cursorMode ? "MENU/CURSOR" : "FLIGHT");
}

static void ToggleKeyboard(void)
{
	SetCursorMode(1);
	keyboardMode=!keyboardMode;
	printf("R36S on-screen keyboard: %s\n",keyboardMode ? "ON" : "OFF");
}

static void TypeSelectedKeyboardCharacter(void)
{
	int c=keyboardRows[keyboardY][keyboardX];
	if(keyboardLowerCase && 'A'<=c && c<='Z')
	{
		c+='a'-'A';
	}
	PushChar(c);
}

#ifndef YS_USE_OPENGL_ES2
static void BeginOverlay2D(void)
{
	glMatrixMode(GL_PROJECTION);
	glPushMatrix();
	glLoadIdentity();
	glOrtho(0,ysWid-1,ysHei-1,0,-1,1);
	glMatrixMode(GL_MODELVIEW);
	glPushMatrix();
	glLoadIdentity();
}

static void EndOverlay2D(void)
{
	glMatrixMode(GL_MODELVIEW);
	glPopMatrix();
	glMatrixMode(GL_PROJECTION);
	glPopMatrix();
	glMatrixMode(GL_MODELVIEW);
}

static void DrawModeBanner(void)
{
	if(SDL_GetTicks()>modeBannerUntil)
	{
		return;
	}

	const GLboolean depthWasEnabled=glIsEnabled(GL_DEPTH_TEST);
	glDisable(GL_DEPTH_TEST);
	BeginOverlay2D();
	YsGlBlitFontString2D(
	    8,20,
	    cursorMode ? "MENU MODE  A:CLICK  SELECT:FLY" : "FLIGHT MODE  SELECT:CURSOR",
	    YsFont8x12,8,12,1.0f,1.0f,0.2f);
	EndOverlay2D();
	if(depthWasEnabled)
	{
		glEnable(GL_DEPTH_TEST);
	}
}

/* The Rockchip display controller exposes no cursor plane, so SDL's hardware
   cursor never reaches the screen.  Draw the pointer with the scene instead. */
static void DrawSoftwareCursor(void)
{
	if(!cursorMode || !mouseCursorVisible)
	{
		return;
	}

	static const int arrow[7][2]=
	{
		{0,0},{0,17},{4,13},{7,19},{10,17},{7,11},{12,11}
	};

	const GLboolean depthWasEnabled=glIsEnabled(GL_DEPTH_TEST);
	const GLboolean blendWasEnabled=glIsEnabled(GL_BLEND);
	const GLboolean textureWasEnabled=glIsEnabled(GL_TEXTURE_2D);
	const GLboolean lightingWasEnabled=glIsEnabled(GL_LIGHTING);
	glDisable(GL_DEPTH_TEST);
	glDisable(GL_BLEND);
	/* Over the 3D scene the scene's texture and lighting would otherwise tint
	   the pointer into invisibility. */
	glDisable(GL_TEXTURE_2D);
	glDisable(GL_LIGHTING);
	BeginOverlay2D();

	/* Black body first, then a smaller white one on top, so the pointer stays
	   readable over both the blue menus and a bright sky. */
	for(int pass=0; pass<2; ++pass)
	{
		const double scale=(0==pass ? 1.0 : 0.72);
		if(0==pass)
		{
			glColor3f(0.0f,0.0f,0.0f);
		}
		else
		{
			glColor3f(1.0f,1.0f,1.0f);
		}
		glBegin(GL_TRIANGLE_FAN);
		for(int i=0; i<7; ++i)
		{
			glVertex2i(lastMx+(int)(arrow[i][0]*scale)+(0==pass ? 0 : 1),
			           lastMy+(int)(arrow[i][1]*scale)+(0==pass ? 0 : 1));
		}
		glEnd();
	}
	glColor3f(1.0f,1.0f,1.0f);

	EndOverlay2D();
	if(depthWasEnabled)
	{
		glEnable(GL_DEPTH_TEST);
	}
	if(blendWasEnabled)
	{
		glEnable(GL_BLEND);
	}
	if(textureWasEnabled)
	{
		glEnable(GL_TEXTURE_2D);
	}
	if(lightingWasEnabled)
	{
		glEnable(GL_LIGHTING);
	}
}

static void DrawOnScreenKeyboard(void)
{
	if(!keyboardMode)
	{
		return;
	}

	const GLboolean depthWasEnabled=glIsEnabled(GL_DEPTH_TEST);
	const GLboolean blendWasEnabled=glIsEnabled(GL_BLEND);

	glMatrixMode(GL_PROJECTION);
	glPushMatrix();
	glLoadIdentity();
	glOrtho(0,ysWid-1,ysHei-1,0,-1,1);
	glMatrixMode(GL_MODELVIEW);
	glPushMatrix();
	glLoadIdentity();
	glDisable(GL_DEPTH_TEST);
	glEnable(GL_BLEND);
	glBlendFunc(GL_SRC_ALPHA,GL_ONE_MINUS_SRC_ALPHA);

	const int panelTop=ysHei-112;
	glColor4f(0.02f,0.02f,0.04f,0.90f);
	glBegin(GL_QUADS);
	glVertex2i(0,panelTop);
	glVertex2i(ysWid,panelTop);
	glVertex2i(ysWid,ysHei);
	glVertex2i(0,ysHei);
	glEnd();

	YsGlBlitFontString2D(
	    8,panelTop+15,"ON-SCREEN KEYBOARD  A:TYPE X:BACKSPACE Y:CASE START:DONE",
	    YsFont8x12,8,12,1.0f,1.0f,0.2f);

	for(int row=0; row<4; ++row)
	{
		char line[64],*ptr=line;
		for(int col=0; col<11; ++col)
		{
			int c=keyboardRows[row][col];
			if(keyboardLowerCase && 'A'<=c && c<='Z')
			{
				c+='a'-'A';
			}
			if(row==keyboardY && col==keyboardX)
			{
				*ptr++='[';
				*ptr++=(char)c;
				*ptr++=']';
			}
			else
			{
				*ptr++=' ';
				*ptr++=(char)c;
				*ptr++=' ';
			}
		}
		*ptr=0;
		YsGlBlitFontString2D(
		    12,panelTop+35+row*18,line,YsFont8x12,8,12,
		    row==keyboardY ? 0.2f : 0.8f,
		    row==keyboardY ? 1.0f : 0.8f,
		    row==keyboardY ? 1.0f : 0.8f);
	}

	glMatrixMode(GL_MODELVIEW);
	glPopMatrix();
	glMatrixMode(GL_PROJECTION);
	glPopMatrix();
	glMatrixMode(GL_MODELVIEW);
	if(depthWasEnabled)
	{
		glEnable(GL_DEPTH_TEST);
	}
	if(!blendWasEnabled)
	{
		glDisable(GL_BLEND);
	}
}
#else
static void DrawOverlayText(int x,int y,const char str[],float r,float g,float b)
{
	auto renderer=YsGLSLSharedBitmapFontRenderer();
	if(nullptr!=renderer)
	{
		YsGLSLUseBitmapFontRenderer(renderer);
		YsGLSLSetBitmapFontRendererViewportOrigin(renderer,YSGLSL_BMPFONT_TOPLEFT_AS_ORIGIN);
		YsGLSLSetBitmapFontRendererFirstLineAlignment(renderer,YSGLSL_BMPFONT_ALIGN_TOPLEFT);
		YsGLSLSetBitmapFontRendererViewportSize(renderer,ysWid,ysHei);
		YsGLSLBitmapFontRendererRequestFontSize(renderer,8,12);
		YsGLSLSetBitmapFontRendererColor3f(renderer,r,g,b);
		YsGLSLRenderBitmapFontString2D(renderer,x,y,str);
		YsGLSLEndUseBitmapFontRenderer(renderer);
	}
}

static void DrawOverlayPrimitive(GLenum mode,int nVertex,const GLfloat vertex[],const GLfloat color[4])
{
	auto renderer=YsGLSLSharedPlain2DRenderer();
	if(nullptr!=renderer)
	{
		YsGLSLUsePlain2DRenderer(renderer);
		YsGLSLUseWindowCoordinateInPlain2DDrawing(renderer,1);
		YsGLSLSetPlain2DRendererUniformColor(renderer,color);
		YsGLSLDrawPlain2DPrimitiveVtxfv(renderer,mode,nVertex,vertex);
		YsGLSLEndUsePlain2DRenderer(renderer);
	}
}

static void DrawModeBanner(void)
{
	if(SDL_GetTicks()<=modeBannerUntil)
	{
		const GLboolean depthWasEnabled=glIsEnabled(GL_DEPTH_TEST);
		glDisable(GL_DEPTH_TEST);
		DrawOverlayText(
		    8,20,
		    cursorMode ? "MENU MODE  A:CLICK  SELECT:FLY" : "FLIGHT MODE  SELECT:CURSOR",
		    1.0f,1.0f,0.2f);
		if(depthWasEnabled)
		{
			glEnable(GL_DEPTH_TEST);
		}
	}
}

static void DrawSoftwareCursor(void)
{
	if(!cursorMode || !mouseCursorVisible)
	{
		return;
	}

	static const GLfloat arrow[14]=
	{
		0,0, 0,17, 4,13, 7,19, 10,17, 7,11, 12,11
	};
	GLfloat vertex[14];
	const GLfloat black[4]={0.0f,0.0f,0.0f,1.0f};
	const GLfloat white[4]={1.0f,1.0f,1.0f,1.0f};
	const GLboolean depthWasEnabled=glIsEnabled(GL_DEPTH_TEST);
	const GLboolean blendWasEnabled=glIsEnabled(GL_BLEND);
	glDisable(GL_DEPTH_TEST);
	glDisable(GL_BLEND);

	for(int pass=0; pass<2; ++pass)
	{
		const GLfloat scale=(0==pass ? 1.0f : 0.72f);
		for(int i=0; i<7; ++i)
		{
			vertex[i*2  ]=(GLfloat)lastMx+arrow[i*2]*scale+(0==pass ? 0.0f : 1.0f);
			vertex[i*2+1]=(GLfloat)lastMy+arrow[i*2+1]*scale+(0==pass ? 0.0f : 1.0f);
		}
		DrawOverlayPrimitive(GL_TRIANGLE_FAN,7,vertex,0==pass ? black : white);
	}

	if(depthWasEnabled)
	{
		glEnable(GL_DEPTH_TEST);
	}
	if(blendWasEnabled)
	{
		glEnable(GL_BLEND);
	}
}

static void DrawOnScreenKeyboard(void)
{
	if(!keyboardMode)
	{
		return;
	}

	const GLboolean depthWasEnabled=glIsEnabled(GL_DEPTH_TEST);
	const GLboolean blendWasEnabled=glIsEnabled(GL_BLEND);
	glDisable(GL_DEPTH_TEST);
	glEnable(GL_BLEND);
	glBlendFunc(GL_SRC_ALPHA,GL_ONE_MINUS_SRC_ALPHA);

	const int panelTop=ysHei-112;
	const GLfloat panel[8]={0,(GLfloat)panelTop, (GLfloat)ysWid,(GLfloat)panelTop, (GLfloat)ysWid,(GLfloat)ysHei, 0,(GLfloat)ysHei};
	const GLfloat panelColor[4]={0.02f,0.02f,0.04f,0.90f};
	DrawOverlayPrimitive(GL_TRIANGLE_FAN,4,panel,panelColor);
	DrawOverlayText(8,panelTop+15,"ON-SCREEN KEYBOARD  A:TYPE X:BACKSPACE Y:CASE START:DONE",1.0f,1.0f,0.2f);

	for(int row=0; row<4; ++row)
	{
		char line[64],*ptr=line;
		for(int col=0; col<11; ++col)
		{
			int c=keyboardRows[row][col];
			if(keyboardLowerCase && 'A'<=c && c<='Z')
			{
				c+='a'-'A';
			}
			if(row==keyboardY && col==keyboardX)
			{
				*ptr++='[';
				*ptr++=(char)c;
				*ptr++=']';
			}
			else
			{
				*ptr++=' ';
				*ptr++=(char)c;
				*ptr++=' ';
			}
		}
		*ptr=0;
		DrawOverlayText(
		    12,panelTop+35+row*18,line,
		    row==keyboardY ? 0.2f : 0.8f,
		    row==keyboardY ? 1.0f : 0.8f,
		    row==keyboardY ? 1.0f : 0.8f);
	}

	if(depthWasEnabled)
	{
		glEnable(GL_DEPTH_TEST);
	}
	if(!blendWasEnabled)
	{
		glDisable(GL_BLEND);
	}
}
#endif

static void ClickMouseButton(int button,int down)
{
	if(SDL_BUTTON_LEFT==button)
	{
		lastLb=down;
		QueueMouseEvent(down ? FSMOUSEEVENT_LBUTTONDOWN : FSMOUSEEVENT_LBUTTONUP);
	}
	else if(SDL_BUTTON_RIGHT==button)
	{
		lastRb=down;
		QueueMouseEvent(down ? FSMOUSEEVENT_RBUTTONDOWN : FSMOUSEEVENT_RBUTTONUP);
	}
}

static void BuildHandheldCursor(void)
{
	/* High-contrast 16x16 arrow.  KMSDRM presents this as a hardware cursor,
	   so it stays responsive even when a menu redraw is expensive. */
	static const char *shape[16]=
	{
		"X...............",
		"XX..............",
		"XOX.............",
		"XOOX............",
		"XOOOX...........",
		"XOOOOX..........",
		"XOOOOOX.........",
		"XOOOOOOX........",
		"XOOOOXXXXX......",
		"XOOXOX..........",
		"XOX.OX..........",
		"XX..OX..........",
		"X....OX.........",
		".....OX.........",
		"......X.........",
		"................"
	};
	SDL_Surface *surface=SDL_CreateRGBSurfaceWithFormat(0,16,16,32,SDL_PIXELFORMAT_RGBA8888);
	if(nullptr!=surface)
	{
		SDL_LockSurface(surface);
		for(int y=0; y<16; ++y)
		{
			auto *row=(Uint32 *)((Uint8 *)surface->pixels+y*surface->pitch);
			for(int x=0; x<16; ++x)
			{
				if('X'==shape[y][x])
				{
					row[x]=SDL_MapRGBA(surface->format,0,0,0,255);
				}
				else if('O'==shape[y][x])
				{
					row[x]=SDL_MapRGBA(surface->format,255,255,255,255);
				}
				else
				{
					row[x]=SDL_MapRGBA(surface->format,0,0,0,0);
				}
			}
		}
		SDL_UnlockSurface(surface);
		ysCursor=SDL_CreateColorCursor(surface,0,0);
		SDL_FreeSurface(surface);
		if(nullptr!=ysCursor)
		{
			SDL_SetCursor(ysCursor);
		}
	}
}

static int MapSdlToFsKey(SDL_Keycode k)
{
	switch (k) {
	case SDLK_ESCAPE: return FSKEY_ESC;
	case SDLK_SPACE: return FSKEY_SPACE;
	case SDLK_RETURN: return FSKEY_ENTER;
	case SDLK_TAB: return FSKEY_TAB;
	case SDLK_BACKSPACE: return FSKEY_BS;
	case SDLK_DELETE: return FSKEY_DEL;
	case SDLK_UP: return FSKEY_UP;
	case SDLK_DOWN: return FSKEY_DOWN;
	case SDLK_LEFT: return FSKEY_LEFT;
	case SDLK_RIGHT: return FSKEY_RIGHT;
	case SDLK_PAGEUP: return FSKEY_PAGEUP;
	case SDLK_PAGEDOWN: return FSKEY_PAGEDOWN;
	case SDLK_HOME: return FSKEY_HOME;
	case SDLK_END: return FSKEY_END;
	case SDLK_INSERT: return FSKEY_INS;
	case SDLK_F1: return FSKEY_F1;
	case SDLK_F2: return FSKEY_F2;
	case SDLK_F3: return FSKEY_F3;
	case SDLK_F4: return FSKEY_F4;
	case SDLK_F5: return FSKEY_F5;
	case SDLK_F6: return FSKEY_F6;
	case SDLK_F7: return FSKEY_F7;
	case SDLK_F8: return FSKEY_F8;
	case SDLK_F9: return FSKEY_F9;
	case SDLK_F10: return FSKEY_F10;
	case SDLK_F11: return FSKEY_F11;
	case SDLK_F12: return FSKEY_F12;
	case SDLK_LSHIFT: case SDLK_RSHIFT: return FSKEY_SHIFT;
	case SDLK_LCTRL: case SDLK_RCTRL: return FSKEY_CTRL;
	case SDLK_LALT: case SDLK_RALT: return FSKEY_ALT;
	default:
		if (k >= SDLK_0 && k <= SDLK_9) return FSKEY_0 + (k - SDLK_0);
		if (k >= SDLK_a && k <= SDLK_z) return FSKEY_A + (k - SDLK_a);
		return FSKEY_NULL;
	}
}

/* Flight-mode button map.  Sticks are read straight from /dev/input/js0 by the
   simulator, so only the digital buttons are translated into key strokes here.
   Holding Select selects the secondary function of each button. */
static int MapFlightButton(int button,int secondary)
{
	if(!secondary)
	{
		switch (button) {
		case SDL_CONTROLLER_BUTTON_A: return FSKEY_SPACE;         /* fire selected weapon */
		case SDL_CONTROLLER_BUTTON_B: return FSKEY_2;             /* cycle weapon */
		case SDL_CONTROLLER_BUTTON_X: return FSKEY_4;             /* dispense flare */
		case SDL_CONTROLLER_BUTTON_Y: return FSKEY_G;             /* landing gear */
		case SDL_CONTROLLER_BUTTON_LEFTSHOULDER: return FSKEY_B;  /* airbrake + wheel brake */
		case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: return FSKEY_TAB;/* afterburner; a held R1 is the throttle gate instead */
		case SDL_CONTROLLER_BUTTON_DPAD_UP: return FSKEY_F1;      /* cockpit view */
		case SDL_CONTROLLER_BUTTON_DPAD_DOWN: return FSKEY_F2;    /* outside view */
		case SDL_CONTROLLER_BUTTON_DPAD_LEFT: return FSKEY_H;     /* look left */
		case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: return FSKEY_K;    /* look right */
		case SDL_CONTROLLER_BUTTON_START: return FSKEY_ESC;       /* in-flight menu */
		default: return FSKEY_NULL;
		}
	}
	switch (button) {
	case SDL_CONTROLLER_BUTTON_A: return FSKEY_3;                 /* radar on/off */
	case SDL_CONTROLLER_BUTTON_B: return FSKEY_BS;                /* autopilot menu */
	case SDL_CONTROLLER_BUTTON_X: return FSKEY_1;                 /* bomb bay door */
	case SDL_CONTROLLER_BUTTON_Y: return FSKEY_T;                 /* auto trim */
	case SDL_CONTROLLER_BUTTON_LEFTSHOULDER: return FSKEY_R;      /* flap retract */
	case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: return FSKEY_F;     /* flap extend */
	case SDL_CONTROLLER_BUTTON_DPAD_UP: return FSKEY_U;           /* look forward */
	case SDL_CONTROLLER_BUTTON_DPAD_DOWN: return FSKEY_M;         /* look back */
	case SDL_CONTROLLER_BUTTON_DPAD_LEFT: return FSKEY_9;         /* HUD visibility */
	case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: return FSKEY_V;        /* velocity indicator */
	default: return FSKEY_NULL;
	}
}

/* Menu-mode button map.  A and X become mouse clicks and are handled separately. */
static int MapMenuButton(int button)
{
	switch (button) {
	case SDL_CONTROLLER_BUTTON_B: return FSKEY_ESC;
	case SDL_CONTROLLER_BUTTON_START: return FSKEY_ESC;
	case SDL_CONTROLLER_BUTTON_Y: return FSKEY_SPACE;             /* activate focused control */
	case SDL_CONTROLLER_BUTTON_LEFTSHOULDER: return FSKEY_TAB;    /* next control */
	case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: return FSKEY_ENTER;
	case SDL_CONTROLLER_BUTTON_DPAD_UP: return FSKEY_UP;
	case SDL_CONTROLLER_BUTTON_DPAD_DOWN: return FSKEY_DOWN;
	case SDL_CONTROLLER_BUTTON_DPAD_LEFT: return FSKEY_LEFT;
	case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: return FSKEY_RIGHT;
	default: return FSKEY_NULL;
	}
}

static void SendKey(int fskey,int down)
{
	if(FSKEY_NULL==fskey)
	{
		return;
	}
	fsKeyPress[fskey]=down;
	if(down)
	{
		PushKey(fskey);
	}
}

static void SendButtonKey(int button,int fskey,int down)
{
	if(down)
	{
		buttonKey[button]=fskey;
		SendKey(fskey,1);
	}
	else
	{
		SendKey(buttonKey[button],0);
		buttonKey[button]=FSKEY_NULL;
	}
}

static void PushKey(int fskey)
{
	if (fskey == FSKEY_NULL) return;
	if (nKeyBufUsed < 256) keyBuffer[nKeyBufUsed++] = fskey;
}

static void PushChar(int c)
{
	if (nCharBufUsed < 256) charBuffer[nCharBufUsed++] = c;
}

static void SetupDefaultGlState(int sizX, int sizY)
{
	glEnable(GL_DEPTH_TEST);
	glDepthFunc(GL_LEQUAL);
#ifndef YS_USE_OPENGL_ES2
	glShadeModel(GL_SMOOTH);

	GLfloat dif[] = {0.8F, 0.8F, 0.8F, 1.0F};
	GLfloat amb[] = {0.4F, 0.4F, 0.4F, 1.0F};
	GLfloat spc[] = {0.9F, 0.9F, 0.9F, 1.0F};
	GLfloat shininess[] = {50.0, 50.0, 50.0, 0.0};

	glEnable(GL_LIGHTING);
	glEnable(GL_LIGHT0);
	glLightfv(GL_LIGHT0, GL_DIFFUSE, dif);
	glLightfv(GL_LIGHT0, GL_SPECULAR, spc);
	glMaterialfv(GL_FRONT | GL_BACK, GL_SHININESS, shininess);
	glLightModelfv(GL_LIGHT_MODEL_AMBIENT, amb);
	glEnable(GL_COLOR_MATERIAL);
	glEnable(GL_NORMALIZE);
#endif

	glClearColor(1.0F, 1.0F, 1.0F, 0.0F);
#ifdef YS_USE_OPENGL_ES2
	glClearDepthf(1.0F);
#else
	glClearDepth(1.0F);
#endif
	glDisable(GL_DEPTH_TEST);
	glViewport(0, 0, sizX, sizY);
#ifndef YS_USE_OPENGL_ES2
	glMatrixMode(GL_PROJECTION);
	glLoadIdentity();
	glOrtho(0, (float)sizX - 1, (float)sizY - 1, 0, -1, 1);
	glMatrixMode(GL_MODELVIEW);
	glLoadIdentity();
	glShadeModel(GL_FLAT);
	glPointSize(1);
#endif
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
#ifndef YS_USE_OPENGL_ES2
	glColor3ub(0, 0, 0);
#endif
}

void FsOpenWindow(const FsOpenWindowOption &opt)
{
	memset(fsKeyPress, 0, sizeof(fsKeyPress));
	memset(buttonKey, 0, sizeof(buttonKey));

	if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER | SDL_INIT_JOYSTICK) != 0) {
		fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
		exit(1);
	}
	printf("SDL video driver=%s\n", SDL_GetCurrentVideoDriver());

	SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, opt.useDoubleBuffer ? 1 : 0);
	SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 16);
#ifdef YS_USE_OPENGL_ES2
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK,SDL_GL_CONTEXT_PROFILE_ES);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION,2);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION,0);
#else
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 1);
#endif

	Uint32 flags = SDL_WINDOW_OPENGL | SDL_WINDOW_SHOWN;
	if (opt.sizeOpt == FsOpenWindowOption::FULLSCREEN ||
	    opt.sizeOpt == FsOpenWindowOption::MAXIMIZE_WINDOW) {
		flags |= SDL_WINDOW_FULLSCREEN_DESKTOP;
	}

	ysWid = opt.wid > 0 ? opt.wid : 640;
	ysHei = opt.hei > 0 ? opt.hei : 480;

	ysWin = SDL_CreateWindow(opt.windowTitle ? opt.windowTitle : "YSFlight",
				 SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
				 ysWid, ysHei, flags);
	if (!ysWin) {
		fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
		exit(1);
	}

	ysCtx = SDL_GL_CreateContext(ysWin);
	if (!ysCtx) {
		fprintf(stderr, "SDL_GL_CreateContext failed: %s\n", SDL_GetError());
		exit(1);
	}
	SDL_GL_SetSwapInterval(1);
	SDL_GL_GetDrawableSize(ysWin, &ysWid, &ysHei);
	printf("Created SDL %s window %dx%d\n",
#ifdef YS_USE_OPENGL_ES2
	    "OpenGL ES 2"
#else
	    "OpenGL"
#endif
	    ,ysWid,ysHei);
	printf("GL_VENDOR=%s\n", (const char *)glGetString(GL_VENDOR));
	printf("GL_RENDERER=%s\n", (const char *)glGetString(GL_RENDERER));
	printf("GL_VERSION=%s\n", (const char *)glGetString(GL_VERSION));

	for (int i = 0; i < SDL_NumJoysticks(); ++i) {
		OpenGameController(i);
	}
	BuildHandheldCursor();
	lastMx=ysWid/2;
	lastMy=ysHei/2;
	SDL_WarpMouseInWindow(ysWin,lastMx,lastMy);
	lastControllerTick=SDL_GetTicks();
	SetCursorMode(1);

	SetupDefaultGlState(ysWid, ysHei);

	if (fsOpenGLInitializationCallBack) {
		(*fsOpenGLInitializationCallBack)(fsOpenGLInitializationCallBackParam);
	}
	if (fsAfterWindowCreationCallBack) {
		(*fsAfterWindowCreationCallBack)(fsAfterWindowCreationCallBackParam);
	}
	exposure = 1;
}

void FsCloseWindow(void)
{
	for (int i = 0; i < YS_MAX_PAD; ++i) {
		if (nullptr != ysPads[i]) {
			SDL_GameControllerClose(ysPads[i]);
			ysPads[i] = nullptr;
		}
	}
	if (ysCursor) {
		SDL_FreeCursor(ysCursor);
		ysCursor = nullptr;
	}
	if (ysCtx) {
		SDL_GL_DeleteContext(ysCtx);
		ysCtx = nullptr;
	}
	if (ysWin) {
		SDL_DestroyWindow(ysWin);
		ysWin = nullptr;
	}
	SDL_Quit();
}

void FsMaximizeWindow(void) {}
void FsUnmaximizeWindow(void) {}
void FsMakeFullScreen(void)
{
	if (ysWin) SDL_SetWindowFullscreen(ysWin, SDL_WINDOW_FULLSCREEN_DESKTOP);
}

void FsResizeWindow(int newWid, int newHei)
{
	if (ysWin) SDL_SetWindowSize(ysWin, newWid, newHei);
}

int FsCheckWindowOpen(void)
{
	return ysWin ? 1 : 0;
}

void FsGetWindowSize(int &wid, int &hei)
{
	wid = ysWid;
	hei = ysHei;
}

void FsGetWindowPosition(int &x0, int &y0)
{
	x0 = 0;
	y0 = 0;
}

void FsSetWindowTitle(const char windowTitle[])
{
	if (ysWin) SDL_SetWindowTitle(ysWin, windowTitle);
}

void FsPollDevice(void)
{
	SDL_Event ev;
	while (SDL_PollEvent(&ev)) {
		switch (ev.type) {
		case SDL_QUIT:
			PushKey(FSKEY_ESC);
			break;
		case SDL_WINDOWEVENT:
			if (ev.window.event == SDL_WINDOWEVENT_EXPOSED ||
			    ev.window.event == SDL_WINDOWEVENT_RESIZED ||
			    ev.window.event == SDL_WINDOWEVENT_SIZE_CHANGED) {
				SDL_GL_GetDrawableSize(ysWin, &ysWid, &ysHei);
				exposure = 1;
			}
			break;
		case SDL_KEYDOWN: {
			int fskey = MapSdlToFsKey(ev.key.keysym.sym);
			if (fskey != FSKEY_NULL) {
				fsKeyPress[fskey] = 1;
				if (!ev.key.repeat) PushKey(fskey);
			}
			break;
		}
		case SDL_KEYUP: {
			int fskey = MapSdlToFsKey(ev.key.keysym.sym);
			if (fskey != FSKEY_NULL) fsKeyPress[fskey] = 0;
			break;
		}
		case SDL_TEXTINPUT:
			for (const char *p = ev.text.text; *p; ++p) PushChar((unsigned char)*p);
			break;
		case SDL_MOUSEMOTION:
			lastMx = ev.motion.x;
			lastMy = ev.motion.y;
			if (nMosBufUsed < 256) {
				mosBuffer[nMosBufUsed++] = {FSMOUSEEVENT_MOVE, lastLb, lastMb, lastRb, lastMx, lastMy};
			}
			break;
		case SDL_MOUSEBUTTONDOWN:
		case SDL_MOUSEBUTTONUP: {
			int down = (ev.type == SDL_MOUSEBUTTONDOWN);
			if (ev.button.button == SDL_BUTTON_LEFT) lastLb = down;
			if (ev.button.button == SDL_BUTTON_MIDDLE) lastMb = down;
			if (ev.button.button == SDL_BUTTON_RIGHT) lastRb = down;
			lastMx = ev.button.x;
			lastMy = ev.button.y;
			int type = FSMOUSEEVENT_NONE;
			if (ev.button.button == SDL_BUTTON_LEFT)
				type = down ? FSMOUSEEVENT_LBUTTONDOWN : FSMOUSEEVENT_LBUTTONUP;
			else if (ev.button.button == SDL_BUTTON_MIDDLE)
				type = down ? FSMOUSEEVENT_MBUTTONDOWN : FSMOUSEEVENT_MBUTTONUP;
			else if (ev.button.button == SDL_BUTTON_RIGHT)
				type = down ? FSMOUSEEVENT_RBUTTONDOWN : FSMOUSEEVENT_RBUTTONUP;
			if (type != FSMOUSEEVENT_NONE && nMosBufUsed < 256) {
				mosBuffer[nMosBufUsed++] = {type, lastLb, lastMb, lastRb, lastMx, lastMy};
			}
			break;
		}
		case SDL_CONTROLLERBUTTONDOWN:
		case SDL_CONTROLLERBUTTONUP: {
			const int down=(ev.type==SDL_CONTROLLERBUTTONDOWN);
			const int button=ev.cbutton.button;

			if(SDL_CONTROLLER_BUTTON_BACK==button)
			{
				selectButtonDown=down;
				if(down)
				{
					selectChordUsed=0;
					selectDownAt=SDL_GetTicks();
				}
				else if(!selectChordUsed &&
				        SDL_GetTicks()-selectDownAt<padTapMs)
				{
					SetCursorMode(!cursorMode);
				}
				break;
			}

			/* Select+Start is the exit hotkey on every other port this handheld
			   runs, and there is otherwise no way out of the game short of
			   steering the cursor through the File menu.  Checked ahead of the
			   keyboard and cursor branches so that it works from any mode. */
			if(down && SDL_CONTROLLER_BUTTON_START==button && selectButtonDown)
			{
				selectChordUsed=1;
				quitRequested=1;
				break;
			}

			/* The keyboard is only ever wanted while the cursor drives menus, so
			   its chord is confined to that mode and leaves Select+Y free to be
			   auto trim in flight. */
			if(down && (SDL_CONTROLLER_BUTTON_GUIDE==button ||
			            (SDL_CONTROLLER_BUTTON_Y==button && selectButtonDown &&
			             (cursorMode || keyboardMode))))
			{
				selectChordUsed=1;
				ToggleKeyboard();
				break;
			}

			/* Some pads come up without a Select mapping, which would leave no
			   way out of cursor mode, so both shoulders together also switch. */
			if(SDL_CONTROLLER_BUTTON_LEFTSHOULDER==button ||
			   SDL_CONTROLLER_BUTTON_RIGHTSHOULDER==button)
			{
				const int other=(SDL_CONTROLLER_BUTTON_LEFTSHOULDER==button ?
				                 SDL_CONTROLLER_BUTTON_RIGHTSHOULDER :
				                 SDL_CONTROLLER_BUTTON_LEFTSHOULDER);
				if(down && FSKEY_NULL!=buttonKey[other])
				{
					SetCursorMode(!cursorMode);
					break;
				}
			}

			/* R1 gates the throttle stick.  A quick tap keeps its old job of
			   toggling the afterburner.  Holding it open must not, even when the
			   stick stayed centred the whole time: the pilot who holds the gate
			   and then decides against a power change has not asked for reheat. */
			if(!cursorMode && !keyboardMode && !selectButtonDown &&
			   SDL_CONTROLLER_BUTTON_RIGHTSHOULDER==button)
			{
				throttleGateDown=down;
				if(down)
				{
					throttleGateUsed=0;
					throttleGateDownAt=SDL_GetTicks();
				}
				else if(!throttleGateUsed &&
				        SDL_GetTicks()-throttleGateDownAt<padTapMs)
				{
					PushKey(FSKEY_TAB);
				}
				break;
			}

			if(keyboardMode)
			{
				if(!down)
				{
					break;
				}
				switch(button)
				{
				case SDL_CONTROLLER_BUTTON_A:
					TypeSelectedKeyboardCharacter();
					break;
				case SDL_CONTROLLER_BUTTON_B:
					keyboardMode=0;
					break;
				case SDL_CONTROLLER_BUTTON_X:
					PushKey(FSKEY_BS);
					break;
				case SDL_CONTROLLER_BUTTON_Y:
					keyboardLowerCase=!keyboardLowerCase;
					break;
				case SDL_CONTROLLER_BUTTON_START:
					PushKey(FSKEY_ENTER);
					keyboardMode=0;
					break;
				case SDL_CONTROLLER_BUTTON_DPAD_LEFT:
					keyboardX=(keyboardX+10)%11;
					break;
				case SDL_CONTROLLER_BUTTON_DPAD_RIGHT:
					keyboardX=(keyboardX+1)%11;
					break;
				case SDL_CONTROLLER_BUTTON_DPAD_UP:
					keyboardY=(keyboardY+3)%4;
					break;
				case SDL_CONTROLLER_BUTTON_DPAD_DOWN:
					keyboardY=(keyboardY+1)%4;
					break;
				default:
					break;
				}
				break;
			}

			if(cursorMode)
			{
				if(SDL_CONTROLLER_BUTTON_A==button)
				{
					ClickMouseButton(SDL_BUTTON_LEFT,down);
				}
				else if(SDL_CONTROLLER_BUTTON_X==button)
				{
					ClickMouseButton(SDL_BUTTON_RIGHT,down);
				}
				else
				{
					SendButtonKey(button,MapMenuButton(button),down);
				}
				break;
			}

			if(down && selectButtonDown)
			{
				selectChordUsed=1;
			}
			SendButtonKey(button,MapFlightButton(button,selectButtonDown),down);
			break;
		}
		case SDL_CONTROLLERDEVICEADDED:
			OpenGameController(ev.cdevice.which);
			break;
		case SDL_CONTROLLERDEVICEREMOVED:
			for(int i=0; i<YS_MAX_PAD; ++i)
			{
				if(nullptr!=ysPads[i] &&
				   SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(ysPads[i]))==ev.cdevice.which)
				{
					SDL_GameControllerClose(ysPads[i]);
					ysPads[i]=nullptr;
				}
			}
			break;
		default:
			break;
		}
	}

	if (AnyPadOpen()) {
		const Uint32 now=SDL_GetTicks();
		const Uint32 dt=std::min<Uint32>(now-lastControllerTick,50);
		lastControllerTick=now;

		if(cursorMode)
		{
			/* Either stick aims the cursor.  The response is quadratic so a
			   small push creeps for pixel accuracy and a full push crosses the
			   screen in about a second. */
			const double vx=
			    CursorSpeedFromAxis(PadAxis(SDL_CONTROLLER_AXIS_RIGHTX))+
			    CursorSpeedFromAxis(PadAxis(SDL_CONTROLLER_AXIS_LEFTX));
			const double vy=
			    CursorSpeedFromAxis(PadAxis(SDL_CONTROLLER_AXIS_RIGHTY))+
			    CursorSpeedFromAxis(PadAxis(SDL_CONTROLLER_AXIS_LEFTY));

			/* L2 crawls for pixel work, R2 skims across the screen. */
			double gain=1.0;
			if(PadAxis(SDL_CONTROLLER_AXIS_TRIGGERLEFT)>16000)
			{
				gain=0.2;
			}
			else if(PadAxis(SDL_CONTROLLER_AXIS_TRIGGERRIGHT)>16000)
			{
				gain=2.2;
			}

			cursorAccumulatorX+=vx*gain*(double)dt/1000.0;
			cursorAccumulatorY+=vy*gain*(double)dt/1000.0;
			const int dx=(int)cursorAccumulatorX;
			const int dy=(int)cursorAccumulatorY;
			if(0!=dx || 0!=dy)
			{
				cursorAccumulatorX-=dx;
				cursorAccumulatorY-=dy;
				lastMx+=dx;
				lastMy+=dy;
				if (lastMx < 0) lastMx = 0;
				if (lastMy < 0) lastMy = 0;
				if (lastMx >= ysWid) lastMx = ysWid - 1;
				if (lastMy >= ysHei) lastMy = ysHei - 1;
				SDL_WarpMouseInWindow(ysWin,lastMx,lastMy);
				QueueMouseEvent(FSMOUSEEVENT_MOVE);
			}
		}
		else
		{
			cursorAccumulatorX=0.0;
			cursorAccumulatorY=0.0;
			PollThrottleStick(now);
		}

		/* L2 and R2 are digital on this hardware but SDL reports them as
		   triggers, so they need edge detection rather than button events. */
		const int newLeftTrigger=
		    (PadAxis(SDL_CONTROLLER_AXIS_TRIGGERLEFT)>16000);
		const int newRightTrigger=
		    (PadAxis(SDL_CONTROLLER_AXIS_TRIGGERRIGHT)>16000);
		if(!cursorMode)
		{
			if(newLeftTrigger!=leftTriggerDown)
			{
				if(newLeftTrigger)
				{
					leftTriggerKey=(selectButtonDown ? FSKEY_5 : FSKEY_RBRACKET);
					if(selectButtonDown)
					{
						selectChordUsed=1;
					}
					SendKey(leftTriggerKey,1);  /* spoiler retract / wheel brake */
				}
				else
				{
					SendKey(leftTriggerKey,0);
					leftTriggerKey=FSKEY_NULL;
				}
			}
			if(newRightTrigger!=rightTriggerDown)
			{
				if(newRightTrigger)
				{
					rightTriggerKey=(selectButtonDown ? FSKEY_6 : FSKEY_LBRACKET);
					if(selectButtonDown)
					{
						selectChordUsed=1;
					}
					SendKey(rightTriggerKey,1); /* spoiler extend / machine gun */
				}
				else
				{
					SendKey(rightTriggerKey,0);
					rightTriggerKey=FSKEY_NULL;
				}
			}
		}
		else if(leftTriggerDown || rightTriggerDown)
		{
			SendKey(leftTriggerKey,0);
			SendKey(rightTriggerKey,0);
			leftTriggerKey=FSKEY_NULL;
			rightTriggerKey=FSKEY_NULL;
		}
		leftTriggerDown=newLeftTrigger;
		rightTriggerDown=newRightTrigger;
	}

	if (fsPollDeviceHook) {
		(*fsPollDeviceHook)(fsPollDeviceHookParam);
	}
}

void FsPushOnPaintEvent(void)
{
	exposure = 1;
}

void FsSleep(int ms)
{
	SDL_Delay(ms);
}

long long int FsPassedTime(void)
{
	static auto t0 = std::chrono::steady_clock::now();
	auto t1 = std::chrono::steady_clock::now();
	auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
	t0 = t1;
	return ms;
}

long long int FsSubSecondTimer(void)
{
	static auto t0 = std::chrono::steady_clock::now();
	auto t1 = std::chrono::steady_clock::now();
	return std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
}

void FsGetMouseState(int &lb, int &mb, int &rb, int &mx, int &my)
{
	lb = lastLb;
	mb = lastMb;
	rb = lastRb;
	mx = lastMx;
	my = lastMy;
}

void FsSetMousePosition(int mx, int my)
{
	lastMx = mx;
	lastMy = my;
	if (ysWin) SDL_WarpMouseInWindow(ysWin, mx, my);
}

int FsGetMouseEvent(int &lb, int &mb, int &rb, int &mx, int &my)
{
	if (nMosBufUsed > 0) {
		auto e = mosBuffer[0];
		for (int i = 0; i < nMosBufUsed - 1; ++i) mosBuffer[i] = mosBuffer[i + 1];
		--nMosBufUsed;
		lb = e.lb; mb = e.mb; rb = e.rb; mx = e.mx; my = e.my;
		return e.eventType;
	}
	lb = lastLb; mb = lastMb; rb = lastRb; mx = lastMx; my = lastMy;
	return FSMOUSEEVENT_NONE;
}

void FsSwapBuffers(void)
{
	if (fsSwapBuffersHook && (*fsSwapBuffersHook)(fsSwapBuffersHookParam)) {
		return;
	}
	DrawModeBanner();
	DrawOnScreenKeyboard();
	DrawSoftwareCursor();
	SDL_GL_SwapWindow(ysWin);
#ifdef YS_USE_OPENGL_ES2
	/* Nothing outside the GLSL renderers is supposed to touch the cached
	   states, but resyncing once a frame keeps a stray call from corrupting
	   every subsequent frame. */
	YsGLInvalidateStateCache();
#endif
}

int FsInkey(void)
{
	if (nKeyBufUsed > 0) {
		int k = keyBuffer[0];
		for (int i = 0; i < nKeyBufUsed - 1; ++i) keyBuffer[i] = keyBuffer[i + 1];
		--nKeyBufUsed;
		return k;
	}
	return FSKEY_NULL;
}

int FsInkeyChar(void)
{
	if (nCharBufUsed > 0) {
		int c = charBuffer[0];
		for (int i = 0; i < nCharBufUsed - 1; ++i) charBuffer[i] = charBuffer[i + 1];
		--nCharBufUsed;
		return c;
	}
	return 0;
}

int FsGetKeyState(int fsKeyCode)
{
	if (fsKeyCode < 0 || fsKeyCode >= FSKEY_NUM_KEYCODE) return 0;
	return fsKeyPress[fsKeyCode];
}

int FsCheckWindowExposure(void)
{
	int e = exposure;
	exposure = 0;
	return e;
}

void FsChangeToProgramDir(void)
{
	char *base = SDL_GetBasePath();
	if (base) {
		chdir(base);
		SDL_free(base);
	}
}

void FsPushKey(int fskey)
{
	PushKey(fskey);
}

void FsPushChar(int c)
{
	PushChar(c);
}

int FsGetNumCurrentTouch(void)
{
	return 0;
}

const FsVec2i *FsGetCurrentTouch(void)
{
	return nullptr;
}

int FsEnableIME(void) { return 0; }
void FsDisableIME(void) {}
int FsIsNativeTextInputAvailable(void) { return 0; }
int FsOpenNativeTextInput(int, int, int, int) { return 0; }
void FsCloseNativeTextInput(void) {}
void FsSetNativeTextInputText(const wchar_t[]) {}
int FsGetNativeTextInputTextLength(void) { return 0; }
void FsGetNativeTextInputText(wchar_t str[], int bufLen)
{
	if (bufLen > 0) str[0] = 0;
}
int FsGetNativeTextInputEvent(void) { return FSNATIVETEXTEVENT_NONE; }

void FsShowMouseCursor(int showFlag)
{
	mouseCursorVisible = showFlag ? 1 : 0;
	SDL_ShowCursor(mouseCursorVisible ? SDL_ENABLE : SDL_DISABLE);
}

int FsIsMouseCursorVisible(void)
{
	return mouseCursorVisible;
}

int FsIsGamepadMenuMode(void)
{
	return cursorMode;
}

int FsIsQuitRequested(void)
{
	return quitRequested;
}

/* Right stick X is the rudder (see ctlassign.cfg).  Holding R1 turns the same
   stick into the throttle, and a diagonal push while reaching for power would
   otherwise put in rudder the pilot never asked for. */
int FsGetSuppressedJoyAxis(void)
{
	return throttleGateDown ? 2 : -1;
}

/* Clipboard stubs — fsguilib expects these X11 helpers on Linux. */
void FsX11GetClipBoardString(long long int &returnLength, char *&returnStr)
{
	returnLength = 0;
	returnStr = nullptr;
}

void FsX11SetClipBoardString(long long int /*length*/, const char /*str*/[])
{
}
