#include "renderfunctions.h"

#include <QOpenGLFramebufferObject>
#include <QApplication>
#include <QDesktopWidget>
#include <QDebug>

#include "project/clip.h"
#include "project/sequence.h"
#include "project/media.h"
#include "project/effect.h"
#include "project/footage.h"
#include "project/transition.h"

#include "ui/collapsiblewidget.h"

#include "playback/audio.h"
#include "playback/playback.h"

#include "io/math.h"
#include "io/config.h"
#include "io/avtogl.h"

#include "panels/timeline.h"
#include "panels/viewer.h"

extern "C" {
	#include <libavformat/avformat.h>
}

GLuint draw_clip(QOpenGLFramebufferObject* fbo, GLuint texture, bool clear) {
	glPushMatrix();
	glLoadIdentity();
	glOrtho(0, 1, 0, 1, -1, 1);

	fbo->bind();

	if (clear) {
		glClear(GL_COLOR_BUFFER_BIT);
	}

	glBindTexture(GL_TEXTURE_2D, texture);
	glBegin(GL_QUADS);
	glTexCoord2f(0, 0); // top left
	glVertex2f(0, 0); // top left
	glTexCoord2f(1, 0); // top right
	glVertex2f(1, 0); // top right
	glTexCoord2f(1, 1); // bottom right
	glVertex2f(1, 1); // bottom right
	glTexCoord2f(0, 1); // bottom left
	glVertex2f(0, 1); // bottom left
	glEnd();
	glBindTexture(GL_TEXTURE_2D, 0);

	glPopMatrix();
	return fbo->texture();
}

void process_effect(Clip* c,
					Effect* e,
					double timecode,
					GLTextureCoords& coords,
					GLuint& composite_texture,
					bool& fbo_switcher,
					bool& texture_failed,
					int data) {
	if (e->is_enabled()) {
		if (e->enable_coords) {
			e->process_coords(timecode, coords, data);
		}
		bool can_process_shaders = (e->enable_shader && shaders_are_enabled);
		if (can_process_shaders || e->enable_superimpose) {
			e->startEffect();
			if (can_process_shaders && e->is_glsl_linked()) {
				e->process_shader(timecode, coords);
				composite_texture = draw_clip(c->fbo[fbo_switcher], composite_texture, true);
				fbo_switcher = !fbo_switcher;
			}
			if (e->enable_superimpose) {
				GLuint superimpose_texture = e->process_superimpose(timecode);
				if (superimpose_texture == 0) {
					qWarning() << "Superimpose texture was nullptr, retrying...";
					texture_failed = true;
				} else {
					if (composite_texture == 0) {
						// if there is no previous texture, just return the superimposes texture
						composite_texture = superimpose_texture;
					} else {
						// if the source texture is not already a framebuffer texture,
						// we'll need to make it one before drawing a superimpose effect on it
						if (composite_texture != c->fbo[0]->texture() && composite_texture != c->fbo[1]->texture()) {
							draw_clip(c->fbo[!fbo_switcher], composite_texture, true);
						}

						composite_texture = draw_clip(c->fbo[!fbo_switcher], superimpose_texture, false);
					}
				}
			}
			e->endEffect();
		}
	}
}

GLuint compose_sequence(ComposeSequenceParams &params) {
	GLuint final_fbo = params.main_buffer;

	Sequence* s = params.seq;
	long playhead = s->playhead;

	if (!params.nests.isEmpty()) {
		for (int i=0;i<params.nests.size();i++) {
			s = params.nests.at(i)->media->to_sequence();
			playhead += params.nests.at(i)->clip_in - params.nests.at(i)->get_timeline_in_with_transition();
			playhead = refactor_frame_number(playhead, params.nests.at(i)->sequence->frame_rate, s->frame_rate);
		}

		if (params.video && params.nests.last()->fbo != nullptr) {
			params.nests.last()->fbo[0]->bind();
			glClear(GL_COLOR_BUFFER_BIT);
			final_fbo = params.nests.last()->fbo[0]->handle();
		}
	}

	int audio_track_count = 0;

	QVector<Clip*> current_clips;

	// loop through clips, find currently active, and sort by track
	for (int i=0;i<s->clips.size();i++) {

		Clip* c = s->clips.at(i);

		if (c != nullptr) {

			// if clip is video and we're processing video
			if ((c->track < 0) == params.video) {

				bool clip_is_active = false;

				// is the clip a "footage" clip?
				if (c->media != nullptr && c->media->get_type() == MEDIA_TYPE_FOOTAGE) {
					Footage* m = c->media->to_footage();

					// does the clip have a valid media source?
					if (!m->invalid && !(c->track >= 0 && !is_audio_device_set())) {

						// is the media process and ready?
						if (m->ready) {
							const FootageStream* ms = m->get_stream_from_file_index(c->track < 0, c->media_stream);

							// does the media have a valid media stream source and is it active?
							if (ms != nullptr && is_clip_active(c, playhead)) {

								// open if not open
								if (!c->open) {
									open_clip(c, !params.rendering);
								}

								clip_is_active = true;

								// increment audio track count
								if (c->track >= 0) audio_track_count++;

							} else if (c->finished_opening) {

								// close the clip if it isn't active anymore
								close_clip(c, false);

							}
						} else {

							// media wasn't ready, schedule a redraw
							params.texture_failed = true;

						}
					}
				} else {
					// if the clip is a nested sequence or null clip, just open it

					if (is_clip_active(c, playhead)) {
						if (!c->open) open_clip(c, !params.rendering);
						clip_is_active = true;
					} else if (c->finished_opening) {
						close_clip(c, false);
					}
				}

				// if the clip is active, added it to "current_clips", sorted by track
				if (clip_is_active) {
					bool added = false;

					// track sorting is only necessary for video clips
					// audio clips are mixed equally, so we skip sorting for those
					if (params.video) {

						// insertion sort by track
						for (int j=0;j<current_clips.size();j++) {
							if (current_clips.at(j)->track < c->track) {
								current_clips.insert(j, c);
								added = true;
								break;
							}
						}

					}

					if (!added) {
						current_clips.append(c);
					}
				}
			}
		}
	}

	if (params.video) {
		// set default coordinates based on the sequence, with 0 in the direct center
		glPushMatrix();
		glLoadIdentity();

		glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);

		int half_width = s->width/2;
		int half_height = s->height/2;
		glOrtho(-half_width, half_width, -half_height, half_height, -1, 10);
	}

	// loop through current clips
	for (int i=0;i<current_clips.size();i++) {
		Clip* c = current_clips.at(i);

		// check if this clip was successfully opened by earlier function
		if (c->media != nullptr && c->media->get_type() == MEDIA_TYPE_FOOTAGE && !c->finished_opening) {
			qWarning() << "Tried to display clip" << i << "but it's closed";
			params.texture_failed = true;
		} else {
			// if clip is a video clip
			if (c->track < 0) {
				// reset OpenGL to full color
				glColor4f(1.0, 1.0, 1.0, 1.0);

				// textureID variable contains texture to be drawn on screen at the end
				GLuint textureID = 0;

				// store video source dimensions
				int video_width = c->getWidth();
				int video_height = c->getHeight();

				// if media is footage
				if (c->media != nullptr && c->media->get_type() == MEDIA_TYPE_FOOTAGE) {

					if (c->texture == nullptr) {
						// opengl texture doesn't exist yet, create it

						c->texture = new QOpenGLTexture(QOpenGLTexture::Target2D);
						c->texture->setSize(c->stream->codecpar->width, c->stream->codecpar->height);
						c->texture->setFormat(get_gl_tex_fmt_from_av(c->pix_fmt));
						c->texture->setMipLevels(c->texture->maximumMipLevels());
						c->texture->setMinMagFilters(QOpenGLTexture::Linear, QOpenGLTexture::Linear);
						c->texture->allocateStorage(get_gl_pix_fmt_from_av(c->pix_fmt), QOpenGLTexture::UInt8);
					}

					// retrieve video frame from cache and store it in c->texture
					get_clip_frame(c, qMax(playhead, c->timeline_in), params.texture_failed);

					// retrieve ID from c->texture
					textureID = c->texture->textureId();

					if (textureID == 0) {
						qWarning() << "Failed to create texture";
						return 0;
					}
				}

				// prepare framebuffers for backend drawing operations
				if (c->fbo == nullptr) {
					c->fbo = new QOpenGLFramebufferObject* [2];
					c->fbo[0] = new QOpenGLFramebufferObject(video_width, video_height);
					c->fbo[1] = new QOpenGLFramebufferObject(video_width, video_height);
				}

				// if clip should actually be shown on screen in this frame
				if (playhead >= c->get_timeline_in_with_transition()) {
					glPushMatrix();

					// simple bool for switching between the two framebuffers
					bool fbo_switcher = false;

					glViewport(0, 0, video_width, video_height);

					if (c->media != nullptr && c->media->get_type()== MEDIA_TYPE_SEQUENCE) {
						// for a nested sequence, run this function again on that sequence and retrieve the texture

						// add nested sequence to nest list
						params.nests.append(c);

						// compose sequence
						textureID = compose_sequence(params);

						// remove sequence from nest list
						params.nests.removeLast();

						// compose_sequence() would have written to this clip's fbo[0], so we switch to fbo[1]
						fbo_switcher = true;
					}

					// set up default coordinates for drawing the clip
					GLTextureCoords coords;
					coords.grid_size = 1;
					coords.vertexTopLeftX = coords.vertexBottomLeftX = -video_width/2;
					coords.vertexTopLeftY = coords.vertexTopRightY = -video_height/2;
					coords.vertexTopRightX = coords.vertexBottomRightX = video_width/2;
					coords.vertexBottomLeftY = coords.vertexBottomRightY = video_height/2;
					coords.vertexBottomLeftZ = coords.vertexBottomRightZ = coords.vertexTopLeftZ = coords.vertexTopRightZ = 1;
					coords.textureTopLeftY = coords.textureTopRightY = coords.textureTopLeftX = coords.textureBottomLeftX = 0.0;
					coords.textureBottomLeftY = coords.textureBottomRightY = coords.textureTopRightX = coords.textureBottomRightX = 1.0;
					coords.textureTopLeftQ = coords.textureTopRightQ = coords.textureTopLeftQ = coords.textureBottomLeftQ = 1;
					coords.blendmode = BLEND_MODE_NORMAL;
					coords.opacity = 1.0;

					// if auto-scale is enabled, auto-scale the clip
					if (c->autoscale && (video_width != s->width && video_height != s->height)) {
						float width_multiplier = float(s->width) / float(video_width);
						float height_multiplier = float(s->height) / float(video_height);
						float scale_multiplier = qMin(width_multiplier, height_multiplier);
						glScalef(scale_multiplier, scale_multiplier, 1);
					}

					// == EFFECT CODE START ==

					// get current sequence time in seconds (used for effects)
					double timecode = get_timecode(c, playhead);

					// set up variables for gizmos later
					Effect* first_gizmo_effect = nullptr;
					Effect* selected_effect = nullptr;

					// run through all of the clip's effects
					for (int j=0;j<c->effects.size();j++) {
						Effect* e = c->effects.at(j);
						process_effect(c, e, timecode, coords, textureID, fbo_switcher, params.texture_failed, TA_NO_TRANSITION);

						// retrieve gizmo data from effect
						if (e->are_gizmos_enabled()) {
							if (first_gizmo_effect == nullptr) first_gizmo_effect = e;
							if (e->container->selected) selected_effect = e;
						}
					}

					// using gizmo data, set definitive gizmo
					if (selected_effect != nullptr) {
						(*params.gizmos) = selected_effect;
					} else if (is_clip_selected(c, true)) {
						(*params.gizmos) = first_gizmo_effect;
					}

					// if the clip has an opening transition, process that now
					if (c->get_opening_transition() != nullptr) {
						int transition_progress = playhead - c->get_timeline_in_with_transition();
						if (transition_progress < c->get_opening_transition()->get_length()) {
							process_effect(c, c->get_opening_transition(), double(transition_progress)/double(c->get_opening_transition()->get_length()), coords, textureID, fbo_switcher, params.texture_failed, TA_OPENING_TRANSITION);
						}
					}

					// if the clip has a closing transition, process that now
					if (c->get_closing_transition() != nullptr) {
						int transition_progress = playhead - (c->get_timeline_out_with_transition() - c->get_closing_transition()->get_length());
						if (transition_progress >= 0 && transition_progress < c->get_closing_transition()->get_length()) {
							process_effect(c, c->get_closing_transition(), double(transition_progress)/double(c->get_closing_transition()->get_length()), coords, textureID, fbo_switcher, params.texture_failed, TA_CLOSING_TRANSITION);
						}
					}

					// == EFFECT CODE END ==

					// == START FINAL DRAW ON SEQUENCE BUFFER ==

					// bind framebuffer
					params.ctx->functions()->glBindFramebuffer(GL_DRAW_FRAMEBUFFER, final_fbo);

					// set viewport to sequence size
					glViewport(0, 0, s->width, s->height);

					// bind final texture
					glBindTexture(GL_TEXTURE_2D, textureID);

					// set texture filter to bilinear
					glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
					glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

					// bind and configure blending mode shader
					params.blend_mode_program->bind();
					params.blend_mode_program->setUniformValue("blend_mode", coords.blendmode);
					params.blend_mode_program->setUniformValue("opacity", coords.opacity);

					// draw clip on screen
					glBegin(GL_QUADS);

					if (coords.grid_size <= 1) {
						glTexCoord2f(coords.textureTopLeftX, coords.textureTopLeftY); // top left
						glVertex2f(coords.vertexTopLeftX, coords.vertexTopLeftY); // top left
						glTexCoord2f(coords.textureTopRightX, coords.textureTopRightY); // top right
						glVertex2f(coords.vertexTopRightX, coords.vertexTopRightY); // top right
						glTexCoord2f(coords.textureBottomRightX, coords.textureBottomRightY); // bottom right
						glVertex2f(coords.vertexBottomRightX, coords.vertexBottomRightY); // bottom right
						glTexCoord2f(coords.textureBottomLeftX, coords.textureBottomLeftY); // bottom left
						glVertex2f(coords.vertexBottomLeftX, coords.vertexBottomLeftY); // bottom left
					} else {
						float rows = coords.grid_size;
						float cols = coords.grid_size;

						for (int k=0;k<rows;k++) {
							float row_prog = float(k)/rows;
							float next_row_prog = float(k+1)/rows;
							for (int j=0;j<cols;j++) {
								float col_prog = float(j)/cols;
								float next_col_prog = float(j+1)/cols;

								float vertexTLX = float_lerp(coords.vertexTopLeftX, coords.vertexBottomLeftX, row_prog);
								float vertexTRX = float_lerp(coords.vertexTopRightX, coords.vertexBottomRightX, row_prog);
								float vertexBLX = float_lerp(coords.vertexTopLeftX, coords.vertexBottomLeftX, next_row_prog);
								float vertexBRX = float_lerp(coords.vertexTopRightX, coords.vertexBottomRightX, next_row_prog);

								float vertexTLY = float_lerp(coords.vertexTopLeftY, coords.vertexTopRightY, col_prog);
								float vertexTRY = float_lerp(coords.vertexTopLeftY, coords.vertexTopRightY, next_col_prog);
								float vertexBLY = float_lerp(coords.vertexBottomLeftY, coords.vertexBottomRightY, col_prog);
								float vertexBRY = float_lerp(coords.vertexBottomLeftY, coords.vertexBottomRightY, next_col_prog);

								glTexCoord2f(float_lerp(coords.textureTopLeftX, coords.textureTopRightX, col_prog), float_lerp(coords.textureTopLeftY, coords.textureBottomLeftY, row_prog)); // top left
								glVertex2f(float_lerp(vertexTLX, vertexTRX, col_prog), float_lerp(vertexTLY, vertexBLY, row_prog)); // top left
								glTexCoord2f(float_lerp(coords.textureTopLeftX, coords.textureTopRightX, next_col_prog), float_lerp(coords.textureTopRightY, coords.textureBottomRightY, row_prog)); // top right
								glVertex2f(float_lerp(vertexTLX, vertexTRX, next_col_prog), float_lerp(vertexTRY, vertexBRY, row_prog)); // top right
								glTexCoord2f(float_lerp(coords.textureBottomLeftX, coords.textureBottomRightX, next_col_prog), float_lerp(coords.textureTopRightY, coords.textureBottomRightY, next_row_prog)); // bottom right
								glVertex2f(float_lerp(vertexBLX, vertexBRX, next_col_prog), float_lerp(vertexTRY, vertexBRY, next_row_prog)); // bottom right
								glTexCoord2f(float_lerp(coords.textureBottomLeftX, coords.textureBottomRightX, col_prog), float_lerp(coords.textureTopLeftY, coords.textureBottomLeftY, next_row_prog)); // bottom left
								glVertex2f(float_lerp(vertexBLX, vertexBRX, col_prog), float_lerp(vertexTLY, vertexBLY, next_row_prog)); // bottom left
							}
						}
					}

					glEnd();

					// release blend mode shader
					params.blend_mode_program->release();

					// unbind texture
					glBindTexture(GL_TEXTURE_2D, 0);

					// unbind framebuffer
					params.ctx->functions()->glBindFramebuffer(GL_TEXTURE_2D, 0);

					// == END FINAL DRAW ON SEQUENCE BUFFER ==

					// prepare gizmos
					if ((*params.gizmos) != nullptr
							&& params.nests.isEmpty()
							&& ((*params.gizmos) == first_gizmo_effect
							|| (*params.gizmos) == selected_effect)) {
						(*params.gizmos)->gizmo_draw(timecode, coords); // set correct gizmo coords
						(*params.gizmos)->gizmo_world_to_screen(); // convert gizmo coords to screen coords
					}

					glPopMatrix();
				}
			} else {
				if (params.render_audio || (config.enable_audio_scrubbing && audio_scrub && params.seq->playhead > c->timeline_in)) {
					if (c->media != nullptr && c->media->get_type() == MEDIA_TYPE_SEQUENCE) {
						params.nests.append(c);
						compose_sequence(params);
						params.nests.removeLast();
					} else {
						if (c->lock.tryLock()) {
							// clip is not caching, start caching audio
							cache_clip(c, playhead, c->audio_reset, !params.render_audio, params.nests, params.playback_speed);
							c->lock.unlock();
						}
					}
				}

				// visually update all the keyframe values
				if (c->sequence == params.seq) { // only if you can currently see them
					double ts = (playhead - c->get_timeline_in_with_transition() + c->get_clip_in_with_transition())/s->frame_rate;
					for (int i=0;i<c->effects.size();i++) {
						Effect* e = c->effects.at(i);
						for (int j=0;j<e->row_count();j++) {
							EffectRow* r = e->row(j);
							for (int k=0;k<r->fieldCount();k++) {
								r->field(k)->validate_keyframe_data(ts);
							}
						}
					}
				}
			}
		}
	}

	if (audio_track_count == 0 && params.viewer != nullptr) {
		params.viewer->play_wake();
	}

	if (params.video) {
		glPopMatrix();
	}

	if (!params.nests.isEmpty() && params.nests.last()->fbo != nullptr) {
		// returns nested clip's texture
		return params.nests.last()->fbo[0]->texture();
	}

	return 0;
}

void compose_audio(Viewer* viewer, Sequence* seq, bool render_audio, int playback_speed) {
	ComposeSequenceParams params;
	params.viewer = viewer;
	params.ctx = nullptr;
	params.seq = seq;
	params.video = false;
	params.render_audio = render_audio;
	params.gizmos = nullptr;
	params.rendering = audio_rendering;
	params.playback_speed = playback_speed;
	params.blend_mode_program = nullptr;
	compose_sequence(params);
}
