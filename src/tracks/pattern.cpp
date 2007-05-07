#ifndef SINGLE_SOURCE_COMPILE

/*
 * pattern.cpp - implementation of class pattern which holds notes
 *
 * Copyright (c) 2004-2007 Tobias Doerffel <tobydox/at/users.sourceforge.net>
 * Copyright (c) 2005-2007 Danny McRae <khjklujn/at/yahoo.com>
 * 
 * This file is part of Linux MultiMedia Studio - http://lmms.sourceforge.net
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public
 * License along with this program (see COPYING); if not, write to the
 * Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA 02110-1301 USA.
 *
 */


#include "qt3support.h"

#ifdef QT4

#include <Qt/QtXml>
#include <QtCore/QTimer>
#include <QtGui/QMenu>
#include <QtGui/QMessageBox>
#include <QtGui/QMouseEvent>
#include <QtGui/QProgressBar>
#include <QtGui/QPushButton>

#else

#include <qdom.h>
#include <qpopupmenu.h>
#include <qprogressbar.h>
#include <qpushbutton.h>
#include <qmessagebox.h>
#include <qimage.h>
#include <qtimer.h>

#define addSeparator insertSeparator
#define addMenu insertItem

#endif


#include "pattern.h"
#include "instrument_track.h"
#include "templates.h"
#include "gui_templates.h"
#include "embed.h"
#include "engine.h"
#include "piano_roll.h"
#include "track_container.h"
#include "rename_dialog.h"
#include "sample_buffer.h"
#include "audio_sample_recorder.h"
#include "song_editor.h"
#include "tooltip.h"
#include "bb_editor.h"
#include "string_pair_drag.h"
#include "main_window.h"


QPixmap * pattern::s_stepBtnOn = NULL;
QPixmap * pattern::s_stepBtnOverlay = NULL;
QPixmap * pattern::s_stepBtnOff = NULL;
QPixmap * pattern::s_stepBtnOffLight = NULL;
QPixmap * pattern::s_frozen = NULL;



pattern::pattern ( instrumentTrack * _instrument_track ) :
	trackContentObject( _instrument_track ),
	m_paintPixmap(),
	m_needsUpdate( TRUE ),
	m_instrumentTrack( _instrument_track ),
	m_patternType( BEAT_PATTERN ),
	m_name( _instrument_track->name() ),
	m_steps( DEFAULT_STEPS_PER_TACT ),
	m_frozenPattern( NULL ),
	m_freezing( FALSE ),
	m_freezeAborted( FALSE )
{
	init();
}




pattern::pattern( const pattern & _pat_to_copy ) :
	trackContentObject( _pat_to_copy.m_instrumentTrack ),
	m_paintPixmap(),
	m_needsUpdate( TRUE ),
	m_instrumentTrack( _pat_to_copy.m_instrumentTrack ),
	m_patternType( _pat_to_copy.m_patternType ),
	m_name( "" ),
	m_steps( _pat_to_copy.m_steps ),
	m_frozenPattern( NULL ),
	m_freezeAborted( FALSE )
{
	for( noteVector::const_iterator it = _pat_to_copy.m_notes.begin();
					it != _pat_to_copy.m_notes.end(); ++it )
	{
		m_notes.push_back( new note( **it ) );
	}

	init();
}




pattern::~pattern()
{
	if( engine::getPianoRoll()->currentPattern() == this )
	{
		engine::getPianoRoll()->setCurrentPattern( NULL );
		// we have to have the song-editor to stop playing if it played
		// us before
		if( engine::getSongEditor()->playing() &&
			engine::getSongEditor()->playMode() ==
						songEditor::PLAY_PATTERN )
		{
			engine::getSongEditor()->playPattern( NULL );
		}
	}

	for( noteVector::iterator it = m_notes.begin();
						it != m_notes.end(); ++it )
	{
		delete *it;
	}

	m_notes.clear();

	if( m_frozenPattern )
	{
		sharedObject::unref( m_frozenPattern );
	}
}




void pattern::init( void )
{
	if( s_stepBtnOn == NULL )
	{
		s_stepBtnOn = new QPixmap( embed::getIconPixmap(
							"step_btn_on_100" ) );
	}

	if( s_stepBtnOverlay == NULL )
	{
		s_stepBtnOverlay = new QPixmap( embed::getIconPixmap(
						"step_btn_on_yellow" ) );
	}

	if( s_stepBtnOff == NULL )
	{
		s_stepBtnOff = new QPixmap( embed::getIconPixmap(
							"step_btn_off" ) );
	}

	if( s_stepBtnOffLight == NULL )
	{
		s_stepBtnOffLight = new QPixmap( embed::getIconPixmap(
						"step_btn_off_light" ) );
	}

	if( s_frozen == NULL )
	{
		s_frozen = new QPixmap( embed::getIconPixmap( "frozen" ) );
	}

	saveJournallingState( FALSE );

	ensureBeatNotes();

	changeLength( length() );
	restoreJournallingState();

#ifdef QT3
	// set background-mode for flicker-free redraw
	setBackgroundMode( Qt::NoBackground );
#endif

	setFixedHeight( parentWidget()->height() - 2 );
	setAutoResizeEnabled( FALSE );

	toolTip::add( this,
		tr( "double-click to open this pattern in piano-roll\n"
			"use mouse wheel to set volume of a step" ) );
}




midiTime pattern::length( void ) const
{
	if( m_patternType == BEAT_PATTERN )
	{
		if( m_steps % DEFAULT_STEPS_PER_TACT == 0 )
		{
			return( m_steps * BEATS_PER_TACT );
		}
		return( ( m_steps / DEFAULT_STEPS_PER_TACT + 1 ) *
				DEFAULT_STEPS_PER_TACT * BEATS_PER_TACT );
	}

	Sint32 max_length = 0;

	for( noteVector::const_iterator it = m_notes.begin();
							it != m_notes.end();
									++it )
	{
		max_length = tMax<Sint32>( max_length, ( *it )->endPos() );
	}
	if( max_length % 64 == 0 )
	{
		return( midiTime( tMax<Sint32>( max_length, 64 ) ) );
	}
	return( midiTime( tMax( midiTime( max_length ).getTact() + 1, 1 ),
									0 ) );
}




note * pattern::addNote( const note & _new_note, const bool _quant_pos )
{
	note * new_note = new note( _new_note );
	if( _quant_pos )
	{
		new_note->quantizePos( engine::getPianoRoll()->quantization() );
	}

	engine::getMixer()->lock();
	if( m_notes.size() == 0 || m_notes.back()->pos() <= new_note->pos() )
	{
		m_notes.push_back( new_note );
	}
	else
	{
		// simple algorithm for inserting the note between two 
		// notes with smaller and greater position
		// maybe it could be optimized by starting in the middle and 
		// going forward or backward but note-inserting isn't that
		// time-critical since it is usually not done while playing...
		long new_note_abs_time = new_note->pos();
		noteVector::iterator it = m_notes.begin();

		while( it != m_notes.end() &&
					( *it )->pos() < new_note_abs_time )
		{
			++it;
		}

		m_notes.insert( it, new_note );
	}
	engine::getMixer()->unlock();

	checkType();
	update();
	changeLength( length() );

	updateBBTrack();

	return( new_note );
}




void pattern::removeNote( const note * _note_to_del )
{
	engine::getMixer()->lock();
	noteVector::iterator it = m_notes.begin();
	while( it != m_notes.end() )
	{
		if( *it == _note_to_del )
		{
			delete *it;
			m_notes.erase( it );
			break;
		}
		++it;
	}
	engine::getMixer()->unlock();

	checkType();
	update();
	changeLength( length() );

	updateBBTrack();
}




note * pattern::rearrangeNote( const note * _note_to_proc,
							const bool _quant_pos )
{
	// just rearrange the position of the note by removing it and adding 
	// a copy of it -> addNote inserts it at the correct position
	note copy_of_note( *_note_to_proc );
	removeNote( _note_to_proc );

	return( addNote( copy_of_note, _quant_pos ) );
}




void pattern::clearNotes( void )
{
	engine::getMixer()->lock();
	for( noteVector::iterator it = m_notes.begin(); it != m_notes.end();
									++it )
	{
		delete *it;
	}
	m_notes.clear();
	engine::getMixer()->unlock();

	checkType();
	update();
	if( engine::getPianoRoll()->currentPattern() == this )
	{
		engine::getPianoRoll()->update();
	}
}




void pattern::setType( patternTypes _new_pattern_type )
{
	if( _new_pattern_type == BEAT_PATTERN ||
				_new_pattern_type == MELODY_PATTERN )
	{
		m_patternType = _new_pattern_type;
	}
}




void pattern::checkType( void )
{
	noteVector::iterator it = m_notes.begin();
	while( it != m_notes.end() )
	{
		if( ( *it )->length() > 0 )
		{
			setType( pattern::MELODY_PATTERN );
			return;
		}
		++it;
	}
	setType( pattern::BEAT_PATTERN );
}




void pattern::saveSettings( QDomDocument & _doc, QDomElement & _this )
{
	_this.setAttribute( "type", m_patternType );
	_this.setAttribute( "name", m_name );
	// as the target of copied/dragged pattern is always an existing
	// pattern, we must not store actual position, instead we store -1
	// which tells loadSettings() not to mess around with position
	if( _this.parentNode().nodeName() == "clipboard" ||
			_this.parentNode().nodeName() == "dnddata" )
	{
		_this.setAttribute( "pos", -1 );
	}
	else
	{
		_this.setAttribute( "pos", startPosition() );
	}
	_this.setAttribute( "len", length() );
	_this.setAttribute( "muted", muted() );
	_this.setAttribute( "steps", m_steps );
	_this.setAttribute( "frozen", m_frozenPattern != NULL );

	// now save settings of all notes
	for( noteVector::iterator it = m_notes.begin();
						it != m_notes.end(); ++it )
	{
		if( ( *it )->length() )
		{
			( *it )->saveState( _doc, _this );
		}
	}
}




void pattern::loadSettings( const QDomElement & _this )
{
	unfreeze();

	m_patternType = static_cast<patternTypes>( _this.attribute( "type"
								).toInt() );
	m_name = _this.attribute( "name" );
	if( _this.attribute( "pos" ).toInt() >= 0 )
	{
		movePosition( _this.attribute( "pos" ).toInt() );
	}
	changeLength( midiTime( _this.attribute( "len" ).toInt() ) );
	if( _this.attribute( "muted" ).toInt() != muted() )
	{
		toggleMute();
	}

	clearNotes();

	QDomNode node = _this.firstChild();
	while( !node.isNull() )
	{
		if( node.isElement() &&
			!node.toElement().attribute( "metadata" ).toInt() )
		{
			note * n = new note;
			n->restoreState( node.toElement() );
			m_notes.push_back( n );
		}
		node = node.nextSibling();
        }

	m_steps = _this.attribute( "steps" ).toInt();
	if( m_steps == 0 )
	{
		m_steps = DEFAULT_STEPS_PER_TACT;
	}

	ensureBeatNotes();
	checkType();
/*	if( _this.attribute( "frozen" ).toInt() )
	{
		freeze();
	}*/
	update();
	updateBBTrack();
}



void pattern::update( void )
{
	m_needsUpdate = TRUE;
	changeLength( length() );
	trackContentObject::update();
}




void pattern::openInPianoRoll( void )
{
	openInPianoRoll( FALSE );
}




void pattern::openInPianoRoll( bool )
{
	engine::getPianoRoll()->setCurrentPattern( this );
	engine::getPianoRoll()->show();
	engine::getPianoRoll()->setFocus();
}




void pattern::clear( void )
{
	clearNotes();
	ensureBeatNotes();
}




void pattern::resetName( void )
{
	m_name = m_instrumentTrack->name();
}




void pattern::changeName( void )
{
	renameDialog rename_dlg( m_name );
	rename_dlg.exec();
}




void pattern::freeze( void )
{
	if( engine::getSongEditor()->playing() )
	{
		QMessageBox::information( 0, tr( "Cannot freeze pattern" ),
						tr( "The pattern currently "
							"cannot be freezed "
							"because you're in "
							"play-mode. Please "
							"stop and try again!" ),
						QMessageBox::Ok );
		return;
	}
	if( m_instrumentTrack->muted() || muted() )
	{
		if( QMessageBox::
#if QT_VERSION >= 0x030200		
				 question
#else
				 information
#endif				 
		
					    ( 0, tr( "Pattern muted" ),
						tr( "The track this pattern "
							"belongs to or the "
							"pattern itself is "
							"currently muted "
							"therefore "
							"freezing makes no "
							"sense! Do you still "
							"want to continue?" ),
						QMessageBox::Yes,
						QMessageBox::No |
						QMessageBox::Default |
						QMessageBox::Escape ) ==
			QMessageBox::No )
		{
			return;
		}
	}

	// already frozen?
	if( m_frozenPattern != NULL )
	{
		// then unfreeze, before freezing it again
		unfreeze();
	}

	new patternFreezeThread( this );

}




void pattern::unfreeze( void )
{
	if( m_frozenPattern != NULL )
	{
		sharedObject::unref( m_frozenPattern );
		m_frozenPattern = NULL;
		update();
	}
}




void pattern::abortFreeze( void )
{
	m_freezeAborted = TRUE;
}




#ifdef QT4

void pattern::addSteps( QAction * _item )
{
	addSteps( _item->text().toInt() );
}




void pattern::removeSteps( QAction * _item )
{
	removeSteps( _item->text().toInt() );
}



#else

void pattern::addSteps( QAction * ) { }
void pattern::removeSteps( QAction * ) { }

#endif



void pattern::addSteps( int _n )
{
	m_steps += _n;
	ensureBeatNotes();
	update();
}




void pattern::removeSteps( int _n )
{
	if( _n < m_steps )
	{
		for( int i = m_steps - _n; i < m_steps; ++i )
		{
			for( noteVector::iterator it = m_notes.begin();
						it != m_notes.end(); ++it )
			{
				if( ( *it )->pos() == i * BEATS_PER_TACT &&
							( *it )->length() <= 0 )
				{
					removeNote( *it );
					break;
				}
			}
		}
		m_steps -= _n;
		update();
	}
}




void pattern::constructContextMenu( QMenu * _cm )
{
#ifdef QT4
	QAction * a = new QAction( embed::getIconPixmap( "piano" ),
					tr( "Open in piano-roll" ), _cm );
	_cm->insertAction( _cm->actions()[0], a );
	connect( a, SIGNAL( triggered( bool ) ), this,
					SLOT( openInPianoRoll( bool ) ) );
#else
	_cm->insertItem( embed::getIconPixmap( "piano" ),
					tr( "Open in piano-roll" ),
					this, SLOT( openInPianoRoll() ),
								0, -1, 0 );
#endif
#ifdef QT4
	_cm->insertSeparator( _cm->actions()[1] );
#else
	_cm->insertSeparator( 1 );
#endif

	_cm->addSeparator();

	_cm->addAction( embed::getIconPixmap( "edit_erase" ),
			tr( "Clear all notes" ), this, SLOT( clear() ) );
	_cm->addSeparator();

	_cm->addAction( embed::getIconPixmap( "reload" ), tr( "Reset name" ),
						this, SLOT( resetName() ) );
	_cm->addAction( embed::getIconPixmap( "rename" ), tr( "Change name" ),
						this, SLOT( changeName() ) );
	_cm->addSeparator();

	_cm->addAction( embed::getIconPixmap( "freeze" ),
		( m_frozenPattern != NULL )? tr( "Refreeze" ) : tr( "Freeze" ),
						this, SLOT( freeze() ) );
	_cm->addAction( embed::getIconPixmap( "unfreeze" ), tr( "Unfreeze" ),
						this, SLOT( unfreeze() ) );

	_cm->addSeparator();

#ifdef QT4
	QMenu * add_step_menu = _cm->addMenu(
					embed::getIconPixmap( "step_btn_add" ),
							tr( "Add steps" ) );
	QMenu * remove_step_menu = _cm->addMenu(
				embed::getIconPixmap( "step_btn_remove" ),
							tr( "Remove steps" ) );
	connect( add_step_menu, SIGNAL( triggered( QAction * ) ),
			this, SLOT( addSteps( QAction * ) ) );
	connect( remove_step_menu, SIGNAL( triggered( QAction * ) ),
			this, SLOT( removeSteps( QAction * ) ) );
#else
	QMenu * add_step_menu = new QMenu( this );
	QMenu * remove_step_menu = new QMenu( this );
#endif
	for( int i = 1; i <= 16; i *= 2 )
	{
		const QString label = ( i == 1 ) ?
					tr( "1 step" ) :
					tr( "%1 steps" ).arg( i );
#ifdef QT4
		add_step_menu->addAction( label );
		remove_step_menu->addAction( label );
#else
		int menu_id = add_step_menu->addAction( label, this,
						SLOT( addSteps( int ) ) );
		add_step_menu->setItemParameter( menu_id, i );
		menu_id = remove_step_menu->addAction( label, this,
						SLOT( removeSteps( int ) ) );
		remove_step_menu->setItemParameter( menu_id, i );
#endif
	}
#ifndef QT4
	_cm->addMenu( embed::getIconPixmap( "step_btn_add" ),
					tr( "Add steps" ), add_step_menu );
	_cm->addMenu( embed::getIconPixmap( "step_btn_remove" ),
				tr( "Remove steps" ), remove_step_menu );
#endif
}




void pattern::mouseDoubleClickEvent( QMouseEvent * _me )
{
	if( _me->button() != Qt::LeftButton )
	{
		_me->ignore();
		return;
	}
	if( m_patternType == pattern::MELODY_PATTERN ||
		!( m_patternType == pattern::BEAT_PATTERN &&
		( pixelsPerTact() >= 192 ||
		  			m_steps != DEFAULT_STEPS_PER_TACT ) &&
		_me->y() > height() - s_stepBtnOff->height() ) )
	{
		openInPianoRoll();
	} 
}




void pattern::mousePressEvent( QMouseEvent * _me )
{
	if( _me->button() == Qt::LeftButton &&
		   m_patternType == pattern::BEAT_PATTERN &&
		   ( pixelsPerTact() >= 192 ||
		   m_steps != DEFAULT_STEPS_PER_TACT ) &&
		   _me->y() > height() - s_stepBtnOff->height() )
	{
		int step = ( _me->x() - TCO_BORDER_WIDTH ) *
				length() / BEATS_PER_TACT / width();
		if( step >= m_steps )
		{
			return;
		}
		note * n = m_notes[step];
		if( n->length() < 0 )
		{
			n->setLength( 0 );
		}
		else
		{
			n->setLength( -64 );
		}
		engine::getSongEditor()->setModified();
		update();
		if( engine::getPianoRoll()->currentPattern() == this )
		{
			engine::getPianoRoll()->update();
		}
	}
	else if( m_frozenPattern != NULL && _me->button() == Qt::LeftButton &&
			engine::getMainWindow()->isShiftPressed() == TRUE )
	{
		QString s;
		new stringPairDrag( "sampledata",
					m_frozenPattern->toBase64( s ),
					embed::getIconPixmap( "freeze" ),
					this );
	}
	else
	{
		trackContentObject::mousePressEvent( _me );
	}
}




void pattern::wheelEvent( QWheelEvent * _we )
{
	if( m_patternType == pattern::BEAT_PATTERN &&
		   ( pixelsPerTact() >= 192 ||
		   m_steps != DEFAULT_STEPS_PER_TACT ) &&
		   _we->y() > height() - s_stepBtnOff->height() )
	{
		int step = ( _we->x() - TCO_BORDER_WIDTH ) *
				length() / BEATS_PER_TACT / width();
		if( step >= m_steps )
		{
			return;
		}
		note * n = m_notes[step];
		Uint8 vol = n->getVolume();
		
		if( n->length() == 0 && _we->delta() > 0 )
		{
			n->setLength( -64 );
			n->setVolume( 5 );
		}
		else if( _we->delta() > 0 )
		{
			if( vol < 95 )
			{
				n->setVolume( vol + 5 );
			}
		}
		else
		{
			if( vol > 5 )
			{
				n->setVolume( vol - 5 );
			}
			else
			{
				n->setLength( 0 );
			}
		}
		engine::getSongEditor()->setModified();
		update();
		if( engine::getPianoRoll()->currentPattern() == this )
		{
			engine::getPianoRoll()->update();
		}
		_we->accept();
	}
	else
	{
		trackContentObject::wheelEvent( _we );
	}
}




void pattern::paintEvent( QPaintEvent * )
{
	if( m_needsUpdate == FALSE )
	{
		QPainter p( this );
		p.drawPixmap( 0, 0, m_paintPixmap );
		return;
	}

	changeLength( length() );

	m_needsUpdate = FALSE;

	if( m_paintPixmap.isNull() == TRUE || m_paintPixmap.size() != size() )
	{
		m_paintPixmap = QPixmap( size() );
	}

	QPainter p( &m_paintPixmap );
#ifdef QT4
	QLinearGradient lingrad( 0, 0, 0, height() );
	const QColor c = isSelected() ? QColor( 0, 0, 224 ) :
							QColor( 96, 96, 96 );
	lingrad.setColorAt( 0, c );
	lingrad.setColorAt( 0.5, Qt::black );
	lingrad.setColorAt( 1, c );
	p.fillRect( QRect( 1, 1, width() - 2, height() - 2 ), lingrad );
#else
	for( int y = 1; y < height() / 2; ++y )
	{
		const int gray = 96 - y * 192 / height();
		if( isSelected() == TRUE )
		{
			p.setPen( QColor( 0, 0, 128 + gray ) );
		}
		else
		{
			p.setPen( QColor( gray, gray, gray ) );
		}
		p.drawLine( 1, y, width() - 1, y );
	}
	for( int y = height() / 2; y < height() - 1; ++y )
	{
		const int gray = ( y - height() / 2 ) * 192 / height();
		if( isSelected() == TRUE )
		{
			p.setPen( QColor( 0, 0, 128 + gray ) );
		}
		else
		{
			p.setPen( QColor( gray, gray, gray ) );
		}
		p.drawLine( 1, y, width() - 1, y );
	}
#endif

	p.setPen( QColor( 57, 69, 74 ) );
	p.drawLine( 0, 0, width(), 0 );
	p.drawLine( 0, 0, 0, height() );
	p.setPen( QColor( 120, 130, 140 ) );
	p.drawLine( 0, height() - 1, width() - 1, height() - 1 );
	p.drawLine( width() - 1, 0, width() - 1, height() - 1 );

	p.setPen( QColor( 0, 0, 0 ) );
	p.drawRect( 1, 1, width() - 2, height() - 2 );

	const float ppt = pixelsPerTact();

	if( m_patternType == pattern::MELODY_PATTERN )
	{
		Sint32 central_key = 0;
		if( m_notes.size() > 0 )
		{
			// first determine the central tone so that we can 
			// display the area where most of the m_notes are
			Sint32 total_notes = 0;
			for( noteVector::iterator it = m_notes.begin();
						it != m_notes.end(); ++it )
			{
				if( ( *it )->length() > 0 )
				{
					central_key += ( *it )->key();
					++total_notes;
				}
			}

			if( total_notes > 0 )
			{
				central_key = central_key / total_notes;

				Sint16 central_y = height() / 2;
				Sint16 y_base = central_y + TCO_BORDER_WIDTH -1;

				const Sint16 x_base = TCO_BORDER_WIDTH;

				p.setPen( QColor( 0, 0, 0 ) );
				for( tact tact_num = 1; tact_num <
						length().getTact(); ++tact_num )
				{
					p.drawLine(
						x_base + static_cast<int>(
							ppt * tact_num ) - 1,
						TCO_BORDER_WIDTH,
						x_base + static_cast<int>(
							ppt * tact_num ) - 1,
						height() - 2 *
							TCO_BORDER_WIDTH );
				}
				if( getTrack()->muted() || muted() )
				{
					p.setPen( QColor( 160, 160, 160 ) );
				}
				else if( m_frozenPattern != NULL )
				{
					p.setPen( QColor( 0x00, 0xE0, 0xFF ) );
				}
				else
				{
					p.setPen( QColor( 0xFF, 0xB0, 0x00 ) );
				}

				for( noteVector::iterator it = m_notes.begin();
						it != m_notes.end(); ++it )
				{
					Sint8 y_pos = central_key -
								( *it )->key();

					if( ( *it )->length() > 0 &&
							y_pos > -central_y &&
							y_pos < central_y )
					{
						Sint16 x1 = 2 * x_base +
		static_cast<int>( ( *it )->pos() * ppt / 64 );
						Sint16 x2 = x1 +
			static_cast<int>( ( *it )->length() * ppt / 64 );
						p.drawLine( x1, y_base + y_pos,
							x2, y_base + y_pos );

					}
				}
			}
		}
	}
	else if( m_patternType == pattern::BEAT_PATTERN &&
			( ppt >= 96 || m_steps != DEFAULT_STEPS_PER_TACT ) )
	{
		QPixmap stepon;
		QPixmap stepoverlay;
		QPixmap stepoff;
		QPixmap stepoffl;
		const int steps = length() / BEATS_PER_TACT;
		const int w = width() - 2 * TCO_BORDER_WIDTH;
#ifdef QT4
		stepon = s_stepBtnOn->scaled( w / steps,
					      s_stepBtnOn->height(),
					      Qt::IgnoreAspectRatio,
					      Qt::SmoothTransformation );
		stepoverlay = s_stepBtnOverlay->scaled( w / steps,
					      s_stepBtnOn->height(),
					      Qt::IgnoreAspectRatio,
					      Qt::SmoothTransformation );
		stepoff = s_stepBtnOff->scaled( w / steps,
						s_stepBtnOff->height(),
						Qt::IgnoreAspectRatio,
						Qt::SmoothTransformation );
		stepoffl = s_stepBtnOffLight->scaled( w / steps,
						s_stepBtnOffLight->height(),
						Qt::IgnoreAspectRatio,
						Qt::SmoothTransformation );
#else
		stepon.convertFromImage( 
				s_stepBtnOn->convertToImage().scale(
					w / steps, s_stepBtnOn->height() ) );
		stepoverlay.convertFromImage( 
				s_stepBtnOverlay->convertToImage().scale(
				w / steps, s_stepBtnOverlay->height() ) );
		stepoff.convertFromImage( s_stepBtnOff->convertToImage().scale(
					w / steps, s_stepBtnOff->height() ) );
		stepoffl.convertFromImage( s_stepBtnOffLight->convertToImage().
					scale( w / steps,
						s_stepBtnOffLight->height() ) );
#endif
		for( noteVector::iterator it = m_notes.begin();
						it != m_notes.end(); ++it )
		{
			Sint16 no = ( *it )->pos() / 4;
			Sint16 x = TCO_BORDER_WIDTH + static_cast<int>( no *
								w / steps );
			Sint16 y = height() - s_stepBtnOff->height() - 1;
			
			Uint8 vol = ( *it )->getVolume();
			
			if( ( *it )->length() < 0 )
			{
				p.drawPixmap( x, y, stepoff );
				for( int i = 0; i < vol / 5 + 1; ++i )
				{
					p.drawPixmap( x, y, stepon );
				}
				for( int i = 0; i < ( 25 + ( vol - 75 ) ) / 5;
									++i )
				{
					p.drawPixmap( x, y, stepoverlay );
				}
			}
			else if( ( no / BEATS_PER_TACT ) % 2 )
			{
				p.drawPixmap( x, y, stepoff );
			}
			else
			{
				p.drawPixmap( x, y, stepoffl );
			}
		}
	}

	p.setFont( pointSize<7>( p.font() ) );
	if( muted() || getTrack()->muted() )
	{
		p.setPen( QColor( 192, 192, 192 ) );
	}
	else
	{
		p.setPen( QColor( 32, 240, 32 ) );
	}
	p.drawText( 2, p.fontMetrics().height() - 1, m_name );
	if( muted() )
	{
		p.drawPixmap( 3, p.fontMetrics().height() + 1,
				embed::getIconPixmap( "muted", 16, 16 ) );
	}
	else if( m_frozenPattern != NULL )
	{
		p.setPen( QColor( 0, 224, 255 ) );
		p.drawRect( 0, 0, width(), height() - 1 );
		p.drawPixmap( 3, height() - s_frozen->height() - 4, *s_frozen );
	}

	p.end();

	p.begin( this );
	p.drawPixmap( 0, 0, m_paintPixmap );

}




void pattern::ensureBeatNotes( void )
{
	// make sure, that all step-note exist
	for( int i = 0; i < m_steps; ++i )
	{
		bool found = FALSE;
		for( noteVector::iterator it = m_notes.begin();
						it != m_notes.end(); ++it )
		{
			if( ( *it )->pos() == i * BEATS_PER_TACT &&
							( *it )->length() <= 0 )
			{
				found = TRUE;
				break;
			}
		}
		if( found == FALSE )
		{
			addNote( note( midiTime( 0 ), midiTime( i *
							BEATS_PER_TACT ) ) );
		}
	}
}




void pattern::updateBBTrack( void )
{
	if( getTrack()->getTrackContainer() == engine::getBBEditor() )
	{
		engine::getBBEditor()->updateBBTrack( this );
	}
}




bool pattern::empty( void )
{
	for( noteVector::iterator it = m_notes.begin(); it != m_notes.end();
									++it )
	{
		if( ( *it )->length() != 0 )
		{
			return( FALSE );
		}
	}
	return( TRUE );
}







patternFreezeStatusDialog::patternFreezeStatusDialog( QThread * _thread ) :
	QDialog(),
	m_freezeThread( _thread ),
	m_progress( 0 )
{
	setWindowTitle( tr( "Freezing pattern..." ) );
#if QT_VERSION >= 0x030200
	setModal( TRUE );
#endif

	m_progressBar = new QProgressBar( this );
	m_progressBar->setGeometry( 10, 10, 200, 24 );
#ifdef QT4
	m_progressBar->setMaximum( 100 );
#else
	m_progressBar->setTotalSteps( 100 );
#endif
	m_progressBar->setTextVisible( FALSE );
	m_progressBar->show();
	m_cancelBtn = new QPushButton( embed::getIconPixmap( "cancel" ),
							tr( "Cancel" ), this );
	m_cancelBtn->setGeometry( 50, 38, 120, 28 );
	m_cancelBtn->show();
	connect( m_cancelBtn, SIGNAL( clicked() ), this,
						SLOT( cancelBtnClicked() ) );
	show();

	QTimer * update_timer = new QTimer( this );
	connect( update_timer, SIGNAL( timeout() ),
					this, SLOT( updateProgress() ) );
	update_timer->start( 100 );

#ifdef QT4
	setAttribute( Qt::WA_DeleteOnClose, TRUE );
#else
	setWFlags( getWFlags() | Qt::WDestructiveClose );
#endif
	connect( this, SIGNAL( aborted() ), this, SLOT( reject() ) );

}




patternFreezeStatusDialog::~patternFreezeStatusDialog()
{
	m_freezeThread->wait();
	delete m_freezeThread;
}





void patternFreezeStatusDialog::setProgress( int _p )
{
	m_progress = _p;
}




void patternFreezeStatusDialog::closeEvent( QCloseEvent * _ce )
{
	_ce->ignore();
	cancelBtnClicked();
}




void patternFreezeStatusDialog::cancelBtnClicked( void )
{
	emit( aborted() );
	done( -1 );
}




void patternFreezeStatusDialog::updateProgress( void )
{
	if( m_progress < 0 )
	{
		done( 0 );
	}
	else
	{
#ifdef QT4
		m_progressBar->setValue( m_progress );
#else
		m_progressBar->setProgress( m_progress );
#endif
	}
}








patternFreezeThread::patternFreezeThread( pattern * _pattern ) :
	m_pattern( _pattern )
{
	// create status-dialog
	m_statusDlg = new patternFreezeStatusDialog( this );
	QObject::connect( m_statusDlg, SIGNAL( aborted() ),
					m_pattern, SLOT( abortFreeze() ) );

	start();
}




patternFreezeThread::~patternFreezeThread()
{
	m_pattern->update();
}




void patternFreezeThread::run( void )
{
	// create and install audio-sample-recorder
	bool b;
	// we cannot create local copy, because at a later stage
	// mixer::restoreAudioDevice(...) deletes old audio-dev and thus
	// audioSampleRecorder would be destroyed two times...
	audioSampleRecorder * freeze_recorder = new audioSampleRecorder(
					engine::getMixer()->sampleRate(),
							DEFAULT_CHANNELS, b,
							engine::getMixer() );
	engine::getMixer()->setAudioDevice( freeze_recorder,
					engine::getMixer()->highQuality() );

	// prepare stuff for playing correct things later
	engine::getSongEditor()->playPattern( m_pattern, FALSE );
	songEditor::playPos & ppp = engine::getSongEditor()->getPlayPos(
						songEditor::PLAY_PATTERN );
	ppp.setTact( 0 );
	ppp.setTact64th( 0 );
	ppp.setCurrentFrame( 0 );
	ppp.m_timeLineUpdate = FALSE;

	m_pattern->m_freezeAborted = FALSE;
	m_pattern->m_freezing = TRUE;


	// now render everything
	while( ppp < m_pattern->length() &&
					m_pattern->m_freezeAborted == FALSE )
	{
		freeze_recorder->processNextBuffer();
		m_statusDlg->setProgress( ppp * 100 / m_pattern->length() );
	}
	m_statusDlg->setProgress( 100 );
	// render tails
	while( engine::getMixer()->hasPlayHandles() &&
					m_pattern->m_freezeAborted == FALSE )
	{
		freeze_recorder->processNextBuffer();
	}


	m_pattern->m_freezing = FALSE;

	// reset song-editor settings
	engine::getSongEditor()->stop();
	ppp.m_timeLineUpdate = TRUE;

	// create final sample-buffer if freezing was successful
	if( m_pattern->m_freezeAborted == FALSE )
	{
		freeze_recorder->createSampleBuffer(
						&m_pattern->m_frozenPattern );
	}

	// restore original audio-device
	engine::getMixer()->restoreAudioDevice();

	m_statusDlg->setProgress( -1 );	// we're finished

}





#include "pattern.moc"


#endif
