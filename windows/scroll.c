/*
 * Scroll windows and DCs
 *
 * Copyright  David W. Metcalfe, 1993
 *	      Alex Korobka       1995,1996
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include <stdlib.h>

#include "windef.h"
#include "wingdi.h"
#include "wine/winuser16.h"
#include "winuser.h"
#include "user.h"
#include "win.h"
#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(scroll);

/*************************************************************************
 *             fix_caret
 */
static HWND fix_caret(HWND hWnd, LPRECT lprc, UINT flags)
{
   HWND hCaret = CARET_GetHwnd();

   if( hCaret )
   {
       RECT rc;
       CARET_GetRect( &rc );
       if( hCaret == hWnd ||
          (flags & SW_SCROLLCHILDREN && IsChild(hWnd, hCaret)) )
       {
           POINT pt;
           pt.x = rc.left;
           pt.y = rc.top;
           MapWindowPoints( hCaret, hWnd, (LPPOINT)&rc, 2 );
           if( IntersectRect(lprc, lprc, &rc) )
           {
               HideCaret(hCaret);
               lprc->left = pt.x;
               lprc->top = pt.y;
               return hCaret;
           }
       }
   }
   return 0;
}


/*************************************************************************
 *		ScrollWindow (USER32.@)
 *
 */
BOOL WINAPI ScrollWindow( HWND hwnd, INT dx, INT dy,
                              const RECT *rect, const RECT *clipRect )
{
    return
        (ERROR != ScrollWindowEx( hwnd, dx, dy, rect, clipRect, 0, NULL,
                                    (rect ? 0 : SW_SCROLLCHILDREN) |
                                    SW_INVALIDATE ));
}

/*************************************************************************
 *		ScrollDC (USER.221)
 */
BOOL16 WINAPI ScrollDC16( HDC16 hdc, INT16 dx, INT16 dy, const RECT16 *rect,
                          const RECT16 *cliprc, HRGN16 hrgnUpdate,
                          LPRECT16 rcUpdate )
{
    RECT rect32, clipRect32, rcUpdate32;
    BOOL16 ret;

    if (rect) CONV_RECT16TO32( rect, &rect32 );
    if (cliprc) CONV_RECT16TO32( cliprc, &clipRect32 );
    ret = ScrollDC( hdc, dx, dy, rect ? &rect32 : NULL,
                      cliprc ? &clipRect32 : NULL, hrgnUpdate, &rcUpdate32 );
    if (rcUpdate) CONV_RECT32TO16( &rcUpdate32, rcUpdate );
    return ret;
}


/*************************************************************************
 *		ScrollDC (USER32.@)
 *
 *   Only the hrgnUpdate is return in device coordinate.
 *   rcUpdate must be returned in logical coordinate to comply with win API.
 *
 */
BOOL WINAPI ScrollDC( HDC hdc, INT dx, INT dy, const RECT *rc,
                          const RECT *prLClip, HRGN hrgnUpdate,
                          LPRECT rcUpdate )
{
    if (USER_Driver.pScrollDC)
        return USER_Driver.pScrollDC( hdc, dx, dy, rc, prLClip, hrgnUpdate, rcUpdate );
    return FALSE;
}


/*************************************************************************
 *		ScrollWindowEx (USER32.@)
 *
 * NOTE: Use this function instead of ScrollWindow32
 */
INT WINAPI ScrollWindowEx( HWND hwnd, INT dx, INT dy,
                               const RECT *rect, const RECT *clipRect,
                               HRGN hrgnUpdate, LPRECT rcUpdate,
                               UINT flags )
{
    RECT rc, cliprc;
    INT result;
    
    if (!WIN_IsWindowDrawable( hwnd, TRUE )) return ERROR;
    hwnd = WIN_GetFullHandle( hwnd );

    GetClientRect(hwnd, &rc);
    if (rect) IntersectRect(&rc, &rc, rect);

    if (clipRect) IntersectRect(&cliprc,&rc,clipRect);
    else cliprc = rc;

    if (!IsRectEmpty(&cliprc) && (dx || dy))
    {
        RECT caretrc = rc;
        HWND hwndCaret = fix_caret(hwnd, &caretrc, flags);

	if (USER_Driver.pScrollWindowEx)
            result = USER_Driver.pScrollWindowEx( hwnd, dx, dy, &rc, &cliprc,
                                                  hrgnUpdate, rcUpdate, flags );
	else
	    result = ERROR; /* FIXME: we should have a fallback implementation */
	
        if( hwndCaret )
        {
            SetCaretPos( caretrc.left + dx, caretrc.top + dy );
            ShowCaret(hwndCaret);
        }
    }
    else 
	result = NULLREGION;
    
    return result;
}
