/*
 * Egis Technology Inc. (aka. LighTuning) EH57E driver for libfprint
 * Copyright (C) 2026 Gianluca Meneghetti
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
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#define FP_COMPONENT "egis057e"

#include "drivers_api.h"
#include "egis057e.h"

#include <math.h>
#include <string.h>

/* What the capture state machine is doing between one frame and the next. */
typedef enum {
  PH_BACKGROUND,        /* learning what an untouched sensor looks like */
  PH_WAIT_OFF,          /* waiting for the finger to be lifted */
  PH_WAIT_ON,           /* waiting for the finger to be placed */
  PH_COLLECT,           /* averaging frames into one template */
} Egis057ePhase;

G_DECLARE_FINAL_TYPE (FpiDeviceEgis057e, fpi_device_egis057e, FPI,
                      DEVICE_EGIS057E, FpDevice)

struct _FpiDeviceEgis057e
{
  FpDevice   parent;

  guint8     buf[EGIS057E_BLOCKSIZE];

  /* On the heap, not inline: GObject refuses an instance bigger than 64 KB
     and two inline arrays of 3990 doubles come to 62 KB on their own. */
  gdouble   *background;
  gboolean   have_background;
  /* Media del fondo, tenuta a parte: serve a togliere lo scostamento uniforme
     in frame_distance, e ricalcolarla a ogni fotogramma sarebbe sprecato. */
  gdouble    bg_mean;
  gint       bg_used;

  Egis057ePhase phase;
  gdouble   *accum;
  gint       collected;
  gint       waited;

  /* Solo per la traccia: conta i fotogrammi per stampare una riga ogni tanto
     invece di novanta al secondo. */
  gint       traccia;

  gint       init_idx;

  /* Appoggi rifiutati di fila perche' troppo simili: azzerato a ogni
     appoggio accettato. */
  gint       rejected;

  /* enrolment in progress */
  GPtrArray *templates;         /* of gint8*, one per placement */
  gint       stage;

  /* the template just captured, normalised and quantised */
  gint8      sample[EGIS057E_IMGSIZE];
};

G_DEFINE_TYPE (FpiDeviceEgis057e, fpi_device_egis057e, FP_TYPE_DEVICE)

static const FpIdEntry egis057e_id_table[] = {
  { .vid = 0x1c7a, .pid = 0x057e },
  { .vid = 0, .pid = 0 },
};

/* --------------------------------------------------------------------------
 * Image maths
 * ------------------------------------------------------------------------ */

/*
 * One separable Gaussian blur pass, edges extended rather than wrapped: the
 * frame is 70 pixels wide and wrapping would fold one side of the fingertip
 * onto the other.
 */
static void
blur (const gdouble *src, gdouble *dst, gdouble sigma)
{
  gint radius = (gint) ceil (3.0 * sigma);
  gint n = 2 * radius + 1;
  g_autofree gdouble *kernel = g_new (gdouble, n);
  g_autofree gdouble *tmp = g_new (gdouble, EGIS057E_IMGSIZE);
  gdouble sum = 0.0;

  for (gint i = 0; i < n; i++)
    {
      gdouble x = i - radius;
      kernel[i] = exp (-x * x / (2.0 * sigma * sigma));
      sum += kernel[i];
    }
  for (gint i = 0; i < n; i++)
    kernel[i] /= sum;

  for (gint y = 0; y < EGIS057E_IMGHEIGHT; y++)
    for (gint x = 0; x < EGIS057E_IMGWIDTH; x++)
      {
        gdouble v = 0.0;
        for (gint i = 0; i < n; i++)
          {
            gint sx = CLAMP (x + i - radius, 0, EGIS057E_IMGWIDTH - 1);
            v += kernel[i] * src[y * EGIS057E_IMGWIDTH + sx];
          }
        tmp[y * EGIS057E_IMGWIDTH + x] = v;
      }

  for (gint y = 0; y < EGIS057E_IMGHEIGHT; y++)
    for (gint x = 0; x < EGIS057E_IMGWIDTH; x++)
      {
        gdouble v = 0.0;
        for (gint i = 0; i < n; i++)
          {
            gint sy = CLAMP (y + i - radius, 0, EGIS057E_IMGHEIGHT - 1);
            v += kernel[i] * tmp[sy * EGIS057E_IMGWIDTH + x];
          }
        dst[y * EGIS057E_IMGWIDTH + x] = v;
      }
}

/*
 * Band-pass and normalise, then quantise to signed bytes. See the header for
 * why the band matters and where the two sigmas come from.
 */
static void
make_template (const gdouble *frame, gint8 *out)
{
  g_autofree gdouble *a = g_new (gdouble, EGIS057E_IMGSIZE);
  g_autofree gdouble *b = g_new (gdouble, EGIS057E_IMGSIZE);
  gdouble mean = 0.0, var = 0.0, sd;

  blur (frame, a, EGIS057E_DOG_SIGMA_IN);
  blur (frame, b, EGIS057E_DOG_SIGMA_OUT);

  for (gint i = 0; i < EGIS057E_IMGSIZE; i++)
    {
      a[i] -= b[i];
      mean += a[i];
    }
  mean /= EGIS057E_IMGSIZE;

  for (gint i = 0; i < EGIS057E_IMGSIZE; i++)
    {
      a[i] -= mean;
      var += a[i] * a[i];
    }
  sd = sqrt (var / EGIS057E_IMGSIZE);
  if (sd < 1e-6)
    sd = 1.0;

  for (gint i = 0; i < EGIS057E_IMGSIZE; i++)
    {
      gdouble v = CLAMP (a[i] / sd, -EGIS057E_TPL_CLIP, EGIS057E_TPL_CLIP);
      out[i] = (gint8) lrint (v * EGIS057E_TPL_SCALE);
    }
}

/*
 * Normalised correlation of the overlapping part of two templates, shifted by
 * (dy, dx). Anything smaller than roughly half a frame is refused: a sliver of
 * overlap correlates with almost anything.
 */
static gdouble
correlate_shift (const gint8 *a, const gint8 *b, gint dy, gint dx)
{
  gdouble sa = 0, sb = 0, saa = 0, sbb = 0, sab = 0;
  gint n = 0;

  for (gint y = MAX (0, dy); y < EGIS057E_IMGHEIGHT + MIN (0, dy); y++)
    for (gint x = MAX (0, dx); x < EGIS057E_IMGWIDTH + MIN (0, dx); x++)
      {
        gdouble va = a[y * EGIS057E_IMGWIDTH + x];
        gdouble vb = b[(y - dy) * EGIS057E_IMGWIDTH + (x - dx)];

        sa += va; sb += vb;
        saa += va * va; sbb += vb * vb; sab += va * vb;
        n++;
      }

  if (n < EGIS057E_IMGSIZE / 2)
    return -1.0;

  {
    gdouble cov = sab / n - (sa / n) * (sb / n);
    gdouble va = saa / n - (sa / n) * (sa / n);
    gdouble vb = sbb / n - (sb / n) * (sb / n);

    if (va <= 1e-9 || vb <= 1e-9)
      return -1.0;

    return cov / sqrt (va * vb);
  }
}

/* Best correlation over every shift up to EGIS057E_MAX_SHIFT. The finger is
 * never put down in exactly the same spot twice. */
static gdouble
correlate (const gint8 *a, const gint8 *b)
{
  gdouble best = -1.0;

  for (gint dy = -EGIS057E_MAX_SHIFT; dy <= EGIS057E_MAX_SHIFT; dy++)
    for (gint dx = -EGIS057E_MAX_SHIFT; dx <= EGIS057E_MAX_SHIFT; dx++)
      {
        gdouble v = correlate_shift (a, b, dy, dx);
        if (v > best)
          best = v;
      }

  return best;
}

/*
 * Distanza media assoluta dal fondo, tolto lo scostamento uniforme.
 *
 * La prima versione confrontava i pixel cosi' com'erano, e il 19/08 si e' piantata
 * in modo istruttivo: durante una sessione di verifiche il livello a sensore
 * libero e' passato da 3.9 a 22-29 e non e' piu' sceso, quindi la fase che
 * aspetta il dito staccato non si e' piu' chiusa e il driver e' diventato sordo.
 * La stessa cosa aveva fermato la prima iscrizione a sei appoggi su venti.
 *
 * Non era una deriva lenta ma un gradino, e di quelli che spostano tutta
 * l'immagine della stessa quantita': un cambio di livello continuo dello stadio
 * analogico, non un cambio di trama. Confrontando i pixel dopo aver tolto la
 * differenza fra la media del fotogramma e quella del fondo, quel gradino sparisce
 * per costruzione.
 *
 * Costa poco: sulle catture del 18/08, con dito il minimo resta 27.0 e senza dito
 * il massimo 25.8 (mediana 3.9). Alla soglia di 15 nessuno dei 5616 fotogrammi
 * con dito finisce sotto, e uno solo dei 984 liberi finisce sopra.
 */
static gdouble
frame_distance (FpiDeviceEgis057e *self)
{
  gdouble media = 0.0, sum = 0.0, scarto;

  for (gint i = 0; i < EGIS057E_IMGSIZE; i++)
    media += self->buf[i];
  media /= EGIS057E_IMGSIZE;

  scarto = media - self->bg_mean;

  for (gint i = 0; i < EGIS057E_IMGSIZE; i++)
    sum += fabs ((gdouble) self->buf[i] - scarto - self->background[i]);

  return sum / EGIS057E_IMGSIZE;
}

static gdouble
frame_deviation (FpiDeviceEgis057e *self)
{
  gdouble mean = 0.0, var = 0.0;

  for (gint i = 0; i < EGIS057E_IMGSIZE; i++)
    mean += self->buf[i];
  mean /= EGIS057E_IMGSIZE;

  for (gint i = 0; i < EGIS057E_IMGSIZE; i++)
    {
      gdouble d = self->buf[i] - mean;
      var += d * d;
    }

  return sqrt (var / EGIS057E_IMGSIZE);
}

/* --------------------------------------------------------------------------
 * USB plumbing
 * ------------------------------------------------------------------------ */

static void
send_pkt (FpDevice *dev, const Egis057ePkt *pkt, FpiSsm *ssm)
{
  FpiUsbTransfer *t = fpi_usb_transfer_new (dev);

  fpi_usb_transfer_fill_bulk (t, EGIS057E_EPOUT, pkt->len);
  memcpy (t->buffer, pkt->data, pkt->len);
  t->ssm = ssm;
  fpi_usb_transfer_submit (t, EGIS057E_TIMEOUT, NULL,
                           fpi_ssm_usb_transfer_cb, NULL);
}

/* --------------------------------------------------------------------------
 * Capture state machine
 *
 * One pass through the four states produces one frame. What happens to that
 * frame depends on self->phase, and the machine either loops for another frame
 * or completes.
 * ------------------------------------------------------------------------ */

enum capture_states {
  CAP_REARM,
  CAP_REQUEST,
  CAP_READ,
  CAP_DECIDE,
  CAP_NUM_STATES,
};

static void
capture_read_cb (FpiUsbTransfer *transfer, FpDevice *dev,
                 gpointer user_data, GError *error)
{
  FpiDeviceEgis057e *self = FPI_DEVICE_EGIS057E (dev);

  if (error)
    {
      fpi_ssm_mark_failed (transfer->ssm, error);
      return;
    }

  if (transfer->actual_length < EGIS057E_IMGSIZE)
    {
      fpi_ssm_mark_failed (transfer->ssm,
                           fpi_device_error_new_msg (FP_DEVICE_ERROR_PROTO,
                                                     "short frame: %zu bytes",
                                                     transfer->actual_length));
      return;
    }

  /*
   * Only the first 3990 bytes are pixels; the rest of the 5320 byte block is a
   * constant 117 all the way to the end. There is no padding interleaved with
   * the rows - an earlier version of this driver assumed there was, and that
   * assumption alone made every capture unreadable.
   */
  memcpy (self->buf, transfer->buffer, EGIS057E_IMGSIZE);
  fpi_ssm_next_state (transfer->ssm);
}

static void
capture_run_state (FpiSsm *ssm, FpDevice *dev)
{
  FpiDeviceEgis057e *self = FPI_DEVICE_EGIS057E (dev);

  switch (fpi_ssm_get_cur_state (ssm))
    {
    case CAP_REARM:
      send_pkt (dev, &egis057e_rearm_pkt, ssm);
      break;

    case CAP_REQUEST:
      send_pkt (dev, &egis057e_imgreq_pkt, ssm);
      break;

    case CAP_READ:
      {
        FpiUsbTransfer *t = fpi_usb_transfer_new (dev);

        fpi_usb_transfer_fill_bulk (t, EGIS057E_EPIN, EGIS057E_BLOCKSIZE);
        t->ssm = ssm;
        t->short_is_error = FALSE;
        fpi_usb_transfer_submit (t, EGIS057E_TIMEOUT,
                                 fpi_device_get_cancellable (dev),
                                 capture_read_cb, NULL);
      }
      break;

    case CAP_DECIDE:
      /*
       * La prima iscrizione vera si e' fermata a sei appoggi su venti, con il
       * dito appoggiato cinquanta volte: dal sesto in poi il driver non ha piu'
       * visto niente. La macchina a stati continuava a girare a novanta
       * fotogrammi al secondo, quindi non era il sensore a essersi fermato: era
       * una delle due soglie a non scattare piu'.
       *
       * Senza sapere quale non si aggiusta niente, e non si puo' chiedere altre
       * cinquanta appoggiate al buio. Una riga ogni mezzo secondo con fase e
       * distanza dice se resta sopra soglia (fondo scappato, il sensore sembra
       * sempre toccato) o sotto (il dito non si vede piu').
       */
      if (self->have_background && ++self->traccia % 45 == 0)
        fp_dbg ("traccia: fase %d distanza %.1f soglia %d",
                self->phase, frame_distance (self), EGIS057E_FINGER_THRESHOLD);

      switch (self->phase)
        {
        case PH_BACKGROUND:
          /* A frame this contrasty already has a finger on it; learning it as
             background would blind the detector for the whole session. */
          if (frame_deviation (self) <= EGIS057E_BG_MAX_DEV)
            {
              for (gint i = 0; i < EGIS057E_IMGSIZE; i++)
                self->accum[i] += self->buf[i];
              self->bg_used++;
            }

          if (self->bg_used >= EGIS057E_BG_FRAMES)
            {
              self->bg_mean = 0.0;
              for (gint i = 0; i < EGIS057E_IMGSIZE; i++)
                {
                  self->background[i] = self->accum[i] / self->bg_used;
                  self->bg_mean += self->background[i];
                }
              self->bg_mean /= EGIS057E_IMGSIZE;
              self->have_background = TRUE;
              fpi_ssm_mark_completed (ssm);
            }
          else if (++self->waited > 200)
            {
              fpi_ssm_mark_failed (ssm,
                                   fpi_device_error_new_msg (FP_DEVICE_ERROR_GENERAL,
                                                             "cannot see an untouched sensor"));
            }
          else
            {
              fpi_ssm_jump_to_state (ssm, CAP_REARM);
            }
          break;

        case PH_WAIT_OFF:
          if (frame_distance (self) < EGIS057E_FINGER_THRESHOLD)
            {
              self->phase = PH_WAIT_ON;
            }
          fpi_ssm_jump_to_state (ssm, CAP_REARM);
          break;

        case PH_WAIT_ON:
          if (frame_distance (self) >= EGIS057E_FINGER_THRESHOLD)
            {
              self->phase = PH_COLLECT;
              self->collected = 0;
              memset (self->accum, 0, EGIS057E_IMGSIZE * sizeof (gdouble));
            }
          fpi_ssm_jump_to_state (ssm, CAP_REARM);
          break;

        case PH_COLLECT:
          /* If the finger comes off early, go back to waiting rather than
             averaging half a placement with half a background. */
          if (frame_distance (self) < EGIS057E_FINGER_THRESHOLD)
            {
              self->phase = PH_WAIT_ON;
              fpi_ssm_jump_to_state (ssm, CAP_REARM);
              break;
            }

          for (gint i = 0; i < EGIS057E_IMGSIZE; i++)
            self->accum[i] += self->buf[i] - self->background[i];
          self->collected++;

          if (self->collected >= EGIS057E_AVG_FRAMES)
            {
              g_autofree gdouble *avg = g_new (gdouble, EGIS057E_IMGSIZE);

              for (gint i = 0; i < EGIS057E_IMGSIZE; i++)
                avg[i] = self->accum[i] / self->collected;

              make_template (avg, self->sample);
              fpi_ssm_mark_completed (ssm);
            }
          else
            {
              fpi_ssm_jump_to_state (ssm, CAP_REARM);
            }
          break;

        default:
          fpi_ssm_mark_failed (ssm,
                               fpi_device_error_new_msg (FP_DEVICE_ERROR_GENERAL,
                                                         "bad phase"));
        }
      break;

    default:
      fpi_ssm_mark_failed (ssm,
                           fpi_device_error_new_msg (FP_DEVICE_ERROR_GENERAL,
                                                     "bad capture state"));
    }
}

static FpiSsm *
capture_ssm (FpDevice *dev, Egis057ePhase phase)
{
  FpiDeviceEgis057e *self = FPI_DEVICE_EGIS057E (dev);

  self->phase = phase;
  self->waited = 0;
  self->collected = 0;
  if (phase == PH_BACKGROUND)
    {
      self->bg_used = 0;
      memset (self->accum, 0, EGIS057E_IMGSIZE * sizeof (gdouble));
    }

  return fpi_ssm_new (dev, capture_run_state, CAP_NUM_STATES);
}

/* --------------------------------------------------------------------------
 * Open and close
 * ------------------------------------------------------------------------ */

enum init_states {
  INIT_SEND,
  INIT_RECV,
  INIT_NEXT,
  INIT_NUM_STATES,
};

static void
init_run_state (FpiSsm *ssm, FpDevice *dev)
{
  FpiDeviceEgis057e *self = FPI_DEVICE_EGIS057E (dev);
  const Egis057ePkt *pkt = &egis057e_init_pkts[self->init_idx];

  switch (fpi_ssm_get_cur_state (ssm))
    {
    case INIT_SEND:
      /* len 0 is the flush pseudo-packet: read whatever is pending and move
         on. Without it the replies run one behind for the rest of the
         sequence. */
      if (pkt->len == 0)
        {
          fpi_ssm_jump_to_state (ssm, INIT_RECV);
          break;
        }
      send_pkt (dev, pkt, ssm);
      break;

    case INIT_RECV:
      {
        FpiUsbTransfer *t = fpi_usb_transfer_new (dev);

        fpi_usb_transfer_fill_bulk (t, EGIS057E_EPIN, 64);
        t->ssm = ssm;
        t->short_is_error = FALSE;
        fpi_usb_transfer_submit (t, EGIS057E_TIMEOUT, NULL,
                                 fpi_ssm_usb_transfer_cb, NULL);
      }
      break;

    case INIT_NEXT:
      self->init_idx++;
      if (self->init_idx >= (gint) EGIS057E_INIT_TOTAL)
        fpi_ssm_mark_completed (ssm);
      else
        fpi_ssm_jump_to_state (ssm, INIT_SEND);
      break;

    default:
      fpi_ssm_mark_failed (ssm,
                           fpi_device_error_new_msg (FP_DEVICE_ERROR_GENERAL,
                                                     "bad init state"));
    }
}

static void
open_bg_done (FpiSsm *ssm, FpDevice *dev, GError *error)
{
  fpi_device_open_complete (dev, error);
}

static void
open_init_done (FpiSsm *ssm, FpDevice *dev, GError *error)
{
  if (error)
    {
      fpi_device_open_complete (dev, error);
      return;
    }

  /* The background has to be measured now, while nobody is touching the
     sensor, because every later decision is a distance from it. */
  fpi_ssm_start (capture_ssm (dev, PH_BACKGROUND), open_bg_done);
}

static void
dev_open (FpDevice *dev)
{
  FpiDeviceEgis057e *self = FPI_DEVICE_EGIS057E (dev);
  GError *error = NULL;

  if (!g_usb_device_claim_interface (fpi_device_get_usb_device (dev), 0, 0,
                                     &error))
    {
      fpi_device_open_complete (dev, error);
      return;
    }

  self->init_idx = 0;
  fpi_ssm_start (fpi_ssm_new (dev, init_run_state, INIT_NUM_STATES),
                 open_init_done);
}

static void
dev_close (FpDevice *dev)
{
  GError *error = NULL;

  g_usb_device_release_interface (fpi_device_get_usb_device (dev), 0, 0,
                                  &error);
  fpi_device_close_complete (dev, error);
}

/* --------------------------------------------------------------------------
 * Enrol
 * ------------------------------------------------------------------------ */

static GVariant *
templates_to_variant (GPtrArray *templates)
{
  GVariantBuilder b;

  g_variant_builder_init (&b, G_VARIANT_TYPE ("aay"));

  for (guint i = 0; i < templates->len; i++)
    g_variant_builder_add_value (&b,
                                 g_variant_new_fixed_array (G_VARIANT_TYPE_BYTE,
                                                            g_ptr_array_index (templates, i),
                                                            EGIS057E_IMGSIZE,
                                                            1));

  return g_variant_builder_end (&b);
}

static void enroll_next (FpDevice *dev);

static void
enroll_stage_done (FpiSsm *ssm, FpDevice *dev, GError *error)
{
  FpiDeviceEgis057e *self = FPI_DEVICE_EGIS057E (dev);
  gint8 *tpl;

  if (error)
    {
      g_clear_pointer (&self->templates, g_ptr_array_unref);
      fpi_device_enroll_complete (dev, NULL, error);
      return;
    }

  /*
   * Un appoggio che somiglia troppo a uno gia' preso non porta niente.
   *
   * L'unica leva misurata che alza i punteggi genuini e' il numero di appoggi
   * DISTINTI: uno da' 0.143, due 0.288, mentre quadruplicare i fotogrammi dello
   * stesso appoggio non cambia nulla. Con quindici millimetri quadri di finestra,
   * trenta appoggi tutti sullo stesso punto valgono quanto uno.
   *
   * Chiedere trenta appoggi senza controllare che siano diversi lascia quindi
   * all'utente il compito di ricordarsi di spostare il dito, e il costo di
   * dimenticarsene si paga dopo, come rifiuti in verifica.
   *
   * Sopra la soglia si chiede di rifare, con CENTER_FINGER, che e' il codice che
   * fprintd e GNOME traducono in "sposta un po' il dito".
   *
   * Il contatore serve a non incastrare nessuno: dopo qualche tentativo
   * l'appoggio si prende comunque. Un'iscrizione che non finisce e' peggio di
   * un'iscrizione un po' ridondante.
   */
  if (self->templates->len > 0 && self->rejected < EGIS057E_ENROLL_MAX_RETRIES)
    {
      gdouble simile = -1.0;

      for (guint i = 0; i < self->templates->len; i++)
        {
          gdouble v = correlate (self->sample,
                                 g_ptr_array_index (self->templates, i));
          if (v > simile)
            simile = v;
        }

      if (simile >= EGIS057E_ENROLL_MAX_SIMILARITY)
        {
          self->rejected++;
          fp_dbg ("appoggio troppo simile a uno gia' preso (%.3f), si rifa'",
                  simile);
          fpi_device_enroll_progress (dev, self->stage, NULL,
                                      fpi_device_retry_new (FP_DEVICE_RETRY_CENTER_FINGER));
          enroll_next (dev);
          return;
        }
    }

  self->rejected = 0;

  tpl = g_memdup2 (self->sample, EGIS057E_IMGSIZE);
  g_ptr_array_add (self->templates, tpl);
  self->stage++;

  fpi_device_enroll_progress (dev, self->stage, NULL, NULL);

  if (self->stage >= EGIS057E_NR_ENROLL_STAGES)
    {
      FpPrint *print = NULL;

      fpi_device_get_enroll_data (dev, &print);
      fpi_print_set_type (print, FPI_PRINT_RAW);
      fpi_print_set_device_stored (print, FALSE);
      g_object_set (print, "fpi-data", templates_to_variant (self->templates),
                    NULL);

      g_clear_pointer (&self->templates, g_ptr_array_unref);
      fpi_device_enroll_complete (dev, g_object_ref (print), NULL);
      return;
    }

  enroll_next (dev);
}

static void
enroll_next (FpDevice *dev)
{
  /* Always wait for the finger to come off first: two placements in a row
     without lifting are the same placement, and it is the number of distinct
     placements that decides whether this works at all. */
  fpi_ssm_start (capture_ssm (dev, PH_WAIT_OFF), enroll_stage_done);
}

static void
dev_enroll (FpDevice *dev)
{
  FpiDeviceEgis057e *self = FPI_DEVICE_EGIS057E (dev);

  self->stage = 0;
  g_clear_pointer (&self->templates, g_ptr_array_unref);
  self->templates = g_ptr_array_new_with_free_func (g_free);

  enroll_next (dev);
}

/* --------------------------------------------------------------------------
 * Verify
 * ------------------------------------------------------------------------ */

static void
verify_done (FpiSsm *ssm, FpDevice *dev, GError *error)
{
  FpiDeviceEgis057e *self = FPI_DEVICE_EGIS057E (dev);
  g_autoptr(GVariant) data = NULL;
  g_autoptr(GVariant) child = NULL;
  GVariantIter iter;
  FpPrint *print = NULL;
  gdouble best = -1.0;

  if (error)
    {
      fpi_device_verify_complete (dev, error);
      return;
    }

  fpi_device_get_verify_data (dev, &print);
  g_object_get (print, "fpi-data", &data, NULL);

  if (data == NULL || !g_variant_is_of_type (data, G_VARIANT_TYPE ("aay")))
    {
      fpi_device_verify_complete (dev,
                                  fpi_device_error_new_msg (FP_DEVICE_ERROR_DATA_INVALID,
                                                            "print was not enrolled by this driver"));
      return;
    }

  g_variant_iter_init (&iter, data);
  while ((child = g_variant_iter_next_value (&iter)))
    {
      gsize len = 0;
      const gint8 *tpl = g_variant_get_fixed_array (child,
                                                    &len, 1);
      if (len == EGIS057E_IMGSIZE)
        {
          gdouble v = correlate (self->sample, tpl);
          if (v > best)
            best = v;
        }
      g_clear_pointer (&child, g_variant_unref);
    }

  fp_dbg ("best correlation %.3f, threshold %.2f", best,
          EGIS057E_MATCH_THRESHOLD);

  fpi_device_verify_report (dev,
                            best >= EGIS057E_MATCH_THRESHOLD ?
                            FPI_MATCH_SUCCESS : FPI_MATCH_FAIL,
                            NULL, NULL);
  fpi_device_verify_complete (dev, NULL);
}

static void
dev_verify (FpDevice *dev)
{
  fpi_ssm_start (capture_ssm (dev, PH_WAIT_OFF), verify_done);
}

/* --------------------------------------------------------------------------
 * Boilerplate
 * ------------------------------------------------------------------------ */

static void
fpi_device_egis057e_init (FpiDeviceEgis057e *self)
{
  self->background = g_new0 (gdouble, EGIS057E_IMGSIZE);
  self->accum = g_new0 (gdouble, EGIS057E_IMGSIZE);
}

static void
fpi_device_egis057e_finalize (GObject *object)
{
  FpiDeviceEgis057e *self = FPI_DEVICE_EGIS057E (object);

  g_clear_pointer (&self->templates, g_ptr_array_unref);
  g_clear_pointer (&self->background, g_free);
  g_clear_pointer (&self->accum, g_free);
  G_OBJECT_CLASS (fpi_device_egis057e_parent_class)->finalize (object);
}

static void
fpi_device_egis057e_class_init (FpiDeviceEgis057eClass *klass)
{
  GObjectClass *obj_class = G_OBJECT_CLASS (klass);
  FpDeviceClass *dev_class = FP_DEVICE_CLASS (klass);

  obj_class->finalize = fpi_device_egis057e_finalize;

  dev_class->id = FP_COMPONENT;
  dev_class->full_name = "EgisTec EH57E";
  dev_class->type = FP_DEVICE_TYPE_USB;
  dev_class->id_table = egis057e_id_table;
  dev_class->scan_type = FP_SCAN_TYPE_PRESS;
  dev_class->nr_enroll_stages = EGIS057E_NR_ENROLL_STAGES;

  /*
   * Solo VERIFY: si sa confrontare un dito presentato con un'impronta
   * indicata, e basta.
   *
   * Niente CAPTURE, perche' non si consegna mai un'immagine a chi chiama --
   * quello che esce dal sensore e' un francobollo di 15 mm quadri, buono per
   * la correlazione e per nient'altro.
   *
   * Niente STORAGE: i modelli non restano nel sensore, tornano a fprintd come
   * blob e li tiene lui.
   *
   * Niente IDENTIFY: si potrebbe scorrere l'elenco e correlare uno per uno,
   * ma il margine misurato fra dita diverse (impostori fino a 0.454) non e'
   * abbastanza largo da reggere un confronto contro molte impronte insieme.
   * Chi chiama fara' verify ripetute, che e' la stessa cosa ma con la
   * responsabilita' della soglia in un posto solo.
   *
   * Il valore va messo per forza: fp_device_constructed lo pretende diverso
   * da NONE, e il primo avvio vero e' morto proprio li'.
   */
  dev_class->features = FP_DEVICE_FEATURE_VERIFY;

  dev_class->open = dev_open;
  dev_class->close = dev_close;
  dev_class->enroll = dev_enroll;
  dev_class->verify = dev_verify;
}
