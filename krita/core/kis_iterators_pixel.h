/* This file is part of the KDE project
 *  Copyright (c) 2004 Cyrille Berger <cberger@cberger.net>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.
*/

#ifndef KIS_ITERATORS_PIXEL_H_
#define KIS_ITERATORS_PIXEL_H_

#include "tiles/kis_iterator.h"
#include "kis_iteratorpixeltrait.h"
#include "kis_pixel.h"
#include "kis_strategy_colorspace.h"


/**
 * The pixel iterators are high level iterarators. The lower level iterators merely return a pointer to some memory
 * where a pixel begins; these iterators return KisPixels -- high-level representations of a pixel together with
 * color model, profile and selectedness. You can access individual channels using the KisPixel [] operator, and .
 */ 


class KisHLineIteratorPixel : public KisHLineIterator, public KisIteratorPixelTrait <KisHLineIterator>
{
public:
    KisHLineIteratorPixel( KisPaintDevice *ndevice, KisDataManager *dm, KisDataManager *sel_dm, Q_INT32 x , Q_INT32 y , Q_INT32 w, Q_INT32 offsetx, Q_INT32 offsety, bool writable);

    inline KisHLineIteratorPixel & operator ++() { KisHLineIterator::operator++(); advance(1); return *this;}

    /// Advances a number of pixels until it reaches the end of the line
    KisHLineIteratorPixel & operator+=(int n) { KisHLineIterator::operator+=(n); advance(n); return *this; };

    Q_INT32 x() const { return KisHLineIterator::x() + m_offsetx; }
    Q_INT32 y() const { return KisHLineIterator::y() + m_offsety; }
protected:
    Q_INT32 m_offsetx, m_offsety;
};

class KisVLineIteratorPixel : public KisVLineIterator, public KisIteratorPixelTrait <KisVLineIterator>
{
public:
    KisVLineIteratorPixel( KisPaintDevice *ndevice, KisDataManager *dm, KisDataManager *sel_dm, Q_INT32 xpos , Q_INT32 ypos , Q_INT32 height, Q_INT32 offsetx, Q_INT32 offsety, bool writable);

    inline KisVLineIteratorPixel & operator ++() { KisVLineIterator::operator++(); advance(1); return *this;}

    Q_INT32 x() const { return KisVLineIterator::x() + m_offsetx; }
    Q_INT32 y() const { return KisVLineIterator::y() + m_offsety; }
protected:
    Q_INT32 m_offsetx, m_offsety;
};

class KisRectIteratorPixel : public KisRectIterator, public KisIteratorPixelTrait <KisRectIterator>
{
public:
    KisRectIteratorPixel( KisPaintDevice *ndevice, KisDataManager *dm, KisDataManager *sel_dm, Q_INT32 x, Q_INT32 y, Q_INT32 w, Q_INT32 h, Q_INT32 offsetx, Q_INT32 offsety, bool writable);

    inline KisRectIteratorPixel & operator ++() { KisRectIterator::operator++(); advance(1); return *this;}

    Q_INT32 x() const { return KisRectIterator::x() + m_offsetx; }
    Q_INT32 y() const { return KisRectIterator::y() + m_offsety; }
protected:
    Q_INT32 m_offsetx, m_offsety;
};

#endif
