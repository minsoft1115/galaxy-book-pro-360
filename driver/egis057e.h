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

#ifndef __EGIS057E_H
#define __EGIS057E_H 1

#define EGIS057E_EPOUT 0x01
#define EGIS057E_EPIN  0x82

#define EGIS057E_TIMEOUT 5000

/*
 * Protocol
 *
 * Requests are "EGIS" followed by a command id and its arguments; replies are
 * "SIGE" followed by register and value. Commands seen so far:
 *
 *   0x60 rr 00      read register rr
 *   0x61 rr vv      write vv into register rr
 *   0x62 rr nn      burst read
 *   0x63 rr ...     burst write
 *   0x64 hh ll      ask for an image, hh:ll bytes
 *
 * The reply to a command is short, but the reply to 0x64 is a stream: the
 * sensor keeps sending until it is asked to stop, repeating the same buffer
 * byte for byte until the acquisition is re-armed.
 */
#define EGIS057E_RESPLEN 64

/*
 * Frame geometry, measured rather than assumed.
 *
 * A block on the wire is 5320 bytes. Only the first 70 * 57 = 3990 are
 * pixels: the tail is a constant 117 (standard deviation exactly 0). An
 * earlier model of this device assumed one padding byte in every four,
 * interleaved; the stream has no interleaved padding at all, and removing
 * one byte in four destroyed a quarter of the pixels and shifted the rest.
 *
 * The row stride shows up as autocorrelation peaks at lag 70, 140, 210...
 * and rows 57 and beyond carry no signal.
 */
#define EGIS057E_IMGWIDTH  70
#define EGIS057E_IMGHEIGHT 57
#define EGIS057E_IMGSIZE   (EGIS057E_IMGWIDTH * EGIS057E_IMGHEIGHT)
#define EGIS057E_BLOCKSIZE 5320

/*
 * Why this is not an FpImageDevice.
 *
 * An image device hands its frames to NBIS and is matched by bozorth3, which
 * compares minutiae. This sensor sees 70 x 57 pixels at about 400 dpi, that is
 * 4.3 x 3.6 mm, roughly 15 mm2. At the usual 0.25 to 0.5 minutiae per mm2 that
 * region holds two to four minutiae, and NBIS duly finds three. Bozorth3 needs
 * far more to decide anything: measured on real captures it scored 8 for two
 * takes of the same finger, against a threshold of 40.
 *
 * The obvious escape - stitch several frames taken while the finger slides -
 * does not work here either, and that was measured rather than assumed: over a
 * 25 second pass the phase correlation between distant frames keeps its peak
 * at (0,0) with a quality of 0.94, while the per-frame mean swings from -57 to
 * +123. The skin in contact does not translate, it only changes pressure. The
 * sensor sits inside a five millimetre power button; it is not a swipe sensor.
 *
 * So matching is done here, on the ridge pattern itself, and the driver is a
 * plain FpDevice with its own enroll and verify.
 */

/*
 * Matching.
 *
 * Each template is one placement of the finger, averaged over several frames
 * and band-pass filtered around the ridge frequency. Scoring is the highest
 * normalised correlation between the presented sample and any enrolled
 * template, over all shifts up to EGIS057E_MAX_SHIFT.
 *
 * Measured on five fingers, three placements each, enrolling two placements
 * and verifying with the third:
 *
 *   genuine    min 0.288   mean 0.606   max 0.815
 *   impostor   min 0.068   mean 0.254   max 0.519
 *
 * At 0.55 that is no false accept in twenty impostor comparisons. Twenty
 * comparisons do not measure a false accept rate, but they do show the signal
 * is there.
 *
 * The genuine minimum is governed by how many distinct placements were
 * enrolled, and by nothing else: one placement gave 0.143, two gave 0.288,
 * while quadrupling the samples drawn from those same two placements changed
 * the figure by not one thousandth. Hence one template per placement, and as
 * many placements as the user will sit through.
 */
/*
 * Trenta appoggi, non venti.
 *
 * Venti bastavano a far funzionare la verifica, ma la prima prova vera ha dato
 * 0.625, 0.595 e 0.358 su tre appoggi: uno sotto la soglia, cioe' un rifiuto su
 * tre. Cercando la causa ho misurato due cose.
 *
 * La prima e' che non e' il momento del contatto: confrontando l'inizio di ogni
 * appoggio con il suo centro e la sua fine (atterraggio.py) la differenza media
 * e' 0.015, rumore. L'ipotesi che il driver campionasse il dito mentre stava
 * ancora atterrando era mia, ed era sbagliata.
 *
 * La seconda e' che a cambiare tutto e' QUALE pezzo di polpastrello tocca: sullo
 * stesso dito, appoggi diversi danno 0.816, 0.229 e 0.703 contro gli stessi
 * modelli. Con quindici millimetri quadri di finestra, spostarsi di due
 * millimetri vuol dire fotografare un'altra parte del dito.
 *
 * L'unica leva che alza i genuini e' quindi il numero di appoggi DISTINTI - gia'
 * misurato il 18/08: un appoggio 0.143, due 0.288, mentre quadruplicare i
 * fotogrammi dello stesso appoggio non cambiava niente.
 *
 * Allargare invece lo scorrimento massimo peggiora: a 12 il miglior impostore
 * sale da 0.451 a 0.600, a 16 a 0.611, perche' le creste sono righe quasi
 * parallele e con piu' liberta' si allineano con chiunque.
 */
#define EGIS057E_NR_ENROLL_STAGES 30
/*
 * Sopra questa somiglianza un appoggio e' considerato una ripetizione di uno
 * gia' preso e in iscrizione si chiede di rifarlo.
 *
 * Il valore viene dai dati del 18/08: sullo stesso dito, appoggi realmente
 * diversi danno fra 0.229 e 0.816, mentre due prelievi dallo STESSO appoggio
 * stanno sopra 0.9 (la differenza fra inizio, centro e fine di un appoggio e'
 * 0.015). Fra le due popolazioni 0.85 e' largo: lascia passare tutto quello che
 * e' spostato davvero e ferma solo il dito rimesso identico.
 *
 * Non e' una soglia di sicurezza e non ha niente a che vedere con
 * EGIS057E_MATCH_THRESHOLD: sbagliarla costa qualche appoggio in piu' o qualche
 * modello ridondante, non un accesso indebito.
 */
#define EGIS057E_ENROLL_MAX_SIMILARITY 0.85

/* Dopo tanti rifiuti di fila l'appoggio si prende comunque: un'iscrizione che
   non finisce e' peggio di una un po' ridondante. */
#define EGIS057E_ENROLL_MAX_RETRIES 4

/*
 * Soglia 0.55.
 *
 * Lo 0.50 di partenza era stato tarato su catture con il 42% dei pixel in
 * saturazione, dove genuini e impostori si sovrapponevano e nessuna soglia
 * funzionava: si era scelto il male minore. Corretto il guadagno (vedi
 * EGIS057E_GAIN_VALUE) le due popolazioni si separano.
 *
 * Tutte le osservazioni sul ferro al guadagno corretto, 19/08, due iscrizioni
 * distinte da trenta appoggi:
 *
 *   genuini    0.585  0.672  0.704  0.757  0.817  0.854  0.947
 *   impostori  0.497  0.508  0.520
 *
 * Fra 0.520 e 0.585 c'e' un corridoio vuoto. 0.55 ci sta in mezzo.
 *
 * Un primo tentativo a 0.65 era stato tarato sui soli tre genuini della prima
 * sessione, tutti sopra 0.81, e rifiutava lo 0.585 della seconda: e' il modo
 * classico di cucire una soglia sui dati che si hanno sottomano invece che
 * sulla popolazione.
 *
 * Dieci osservazioni non misurano un tasso di errore, e il margine e' di 65
 * millesimi, sottile. Inoltre l'impostore provato e' il medio della stessa mano,
 * che e' quello che somiglia di piu' e quindi il caso peggiore per il rifiuto ma
 * non per la sicurezza: un dito di un'altra persona non e' mai stato provato.
 */
#define EGIS057E_MATCH_THRESHOLD  0.55
#define EGIS057E_MAX_SHIFT        8

/*
 * Band-pass, as a difference of Gaussians.
 *
 * The ridges repeat every 8.0 pixels, measured from the peak of a Hanning
 * windowed 2D power spectrum. Below that band lies the pressure of the finger,
 * which changes at every placement and says nothing about identity; above it
 * lies thermal noise. Removing both is what opens the margin: on the raw
 * correlation the same finger reached 0.657 and a different one 0.594, which
 * does not separate at all.
 *
 * A Gaussian band in the frequency domain would need an FFT, which libfprint
 * does not carry. A difference of two Gaussian blurs is separable, runs in a
 * few passes over 3990 pixels, and was checked against the frequency domain
 * version on the same captures:
 *
 *   frequency band 0.125 +- 0.045   genuine mean 0.606   impostor max 0.519
 *   DoG 1.2 / 3.5                   genuine mean 0.573   impostor max 0.454
 *
 * Slightly lower on genuine scores, lower still on the impostor ceiling, which
 * is the side that decides the threshold.
 */
#define EGIS057E_DOG_SIGMA_IN   1.2
#define EGIS057E_DOG_SIGMA_OUT  3.5

/* Frames averaged into one template. The finger is still, so averaging only
 * removes noise; 3.4 levels per frame of it fall roughly tenfold over forty. */
#define EGIS057E_AVG_FRAMES 40

/*
 * Analogue front end.
 *
 * Register 0x12 (gain) is four bits wide: writing 0x20 reads back 0x00, and
 * writing 0xff reads back 0x0f. The init sequence leaves it at 0x00, which
 * yields 18 distinct levels out of 256 - so shallow that a finger disappears
 * below the quantisation step. At 0x0a there are around 200 levels with
 * under 2% of pixels at the rails.
 *
 * Register 0x0f (offset) is six bits wide, but only 0x20 is usable: at any
 * other value the whole image goes to 0 or to 255.
 */
#define EGIS057E_REG_GAIN   0x12
#define EGIS057E_REG_OFFSET 0x0f
/*
 * Guadagno 0x01, non 0x0a.
 *
 * Il valore era stato messo a 0x0a perche' a 0x00 il dito non si vedeva, e li'
 * ci si era fermati. Ma quella prova era stata fatta PRIMA di sistemare
 * l'offset: la manopola girata era quella sbagliata, e una volta portato
 * l'offset a 0x20 il guadagno alto non serviva piu' a niente.
 *
 * Misurato il 19/08 con il dito fermo, spazzolando tutti i guadagni a offset
 * 0x20 (research/tuning/exposure-sweep.py):
 *
 *   guadagno 0x00   satura  0.0%   SNR 19.9 dB
 *   guadagno 0x01   satura  1.9%   SNR 20.2 dB   <- massimo
 *   guadagno 0x04   satura 24.0%   SNR 19.1 dB
 *   guadagno 0x0a   satura 42.5%   SNR 16.2 dB   <- quello che si usava
 *   guadagno 0x0f   satura 49.8%   SNR 14.5 dB
 *
 * A 0x0a quasi meta' dei pixel di ogni immagine arrivava tagliata a 0 o a 255.
 * Informazione distrutta all'acquisizione, che nessun confronto puo'
 * recuperare, e distrutta proprio dove il segnale e' forte: quello che
 * sopravvive e' dominato da dove il dito preme, cioe' dalla componente che
 * tutte le dita hanno in comune.
 */
#define EGIS057E_GAIN_VALUE   0x01
#define EGIS057E_OFFSET_VALUE 0x20

/*
 * Finger detection.
 *
 * Measured with the sensor idle and then with a finger resting on it, thirty
 * frames each, taking the per-pixel median:
 *
 *   idle    mean  71.3   spatial deviation 35.8
 *   finger  mean 100.8   spatial deviation 90.6
 *
 *   idle vs idle    mean absolute difference  2.4
 *   finger vs idle  mean absolute difference 80.9
 *
 * So the mean absolute difference from the background separates the two by
 * more than thirty times. A threshold of 15 sits far from both.
 */
#define EGIS057E_FINGER_THRESHOLD 15

/*
 * A frame whose spatial deviation is this high cannot be background: it
 * already has a finger on it. Used to avoid learning a finger as background.
 */
#define EGIS057E_BG_MAX_DEV 60

/* Frames averaged into the background reference. */
#define EGIS057E_BG_FRAMES 8

/* Templates are stored as signed bytes: the pattern is normalised to unit
 * deviation, clipped at four deviations and scaled by 32, which keeps a whole
 * placement in 3990 bytes instead of the 15960 a float array would take. */
#define EGIS057E_TPL_SCALE 32.0
#define EGIS057E_TPL_CLIP  4.0

/*
 * Commands.
 *
 * Packets vary in length, so each one carries its own.
 */
typedef struct
{
  guint8 len;                  /* 0 marks the flush pseudo-packet */
  guint8 data[20];
} Egis057ePkt;

#define EGIS057E_FLUSH { 0, { 0 } }

/*
 * The init sequence, as captured from the vendor driver. The last line is
 * also what re-arms an acquisition on its own.
 */
static const Egis057ePkt egis057e_init_pkts[] = {
  { 7,  { 0x45, 0x47, 0x49, 0x53, 0x60, 0x00, 0x00 } },
  { 7,  { 0x45, 0x47, 0x49, 0x53, 0x60, 0x01, 0x00 } },
  { 7,  { 0x45, 0x47, 0x49, 0x53, 0x61, 0x10, 0xfd } },
  { 7,  { 0x45, 0x47, 0x49, 0x53, 0x61, 0x35, 0x02 } },
  { 7,  { 0x45, 0x47, 0x49, 0x53, 0x61, 0x80, 0x00 } },
  { 7,  { 0x45, 0x47, 0x49, 0x53, 0x60, 0x80, 0x00 } },
  { 7,  { 0x45, 0x47, 0x49, 0x53, 0x61, 0x10, 0xfc } },
  { 9,  { 0x45, 0x47, 0x49, 0x53, 0x63, 0x01, 0x02, 0x0f, 0x03 } },
  { 7,  { 0x45, 0x47, 0x49, 0x53, 0x61, 0x0c, 0x22 } },
  { 7,  { 0x45, 0x47, 0x49, 0x53, 0x61, 0x09, 0x83 } },
  { 13, { 0x45, 0x47, 0x49, 0x53, 0x63, 0x26, 0x06, 0x06, 0x60, 0x06, 0x05,
          0x2f, 0x06 } },
  { 7,  { 0x45, 0x47, 0x49, 0x53, 0x61, 0x10, 0xf4 } },
  { 7,  { 0x45, 0x47, 0x49, 0x53, 0x61, 0x0c, 0x44 } },
  { 7,  { 0x45, 0x47, 0x49, 0x53, 0x61, 0x50, 0x03 } },
  { 7,  { 0x45, 0x47, 0x49, 0x53, 0x60, 0x50, 0x00 } },
  EGIS057E_FLUSH,
  { 7,  { 0x45, 0x47, 0x49, 0x53, 0x60, 0x40, 0x00 } },
  { 18, { 0x45, 0x47, 0x49, 0x53, 0x63, 0x09, 0x0b, 0x83, 0x24, 0x00, 0x44,
          0x0f, 0x08, 0x20, 0x20, 0x00, 0x00, 0x52 } },
  { 13, { 0x45, 0x47, 0x49, 0x53, 0x63, 0x26, 0x06, 0x06, 0x60, 0x06, 0x05,
          0x2f, 0x06 } },
  { 7,  { 0x45, 0x47, 0x49, 0x53, 0x61, 0x23, 0x00 } },
  { 7,  { 0x45, 0x47, 0x49, 0x53, 0x61, 0x24, 0x38 } },
  { 7,  { 0x45, 0x47, 0x49, 0x53, 0x61, 0x20, 0x00 } },
  { 7,  { 0x45, 0x47, 0x49, 0x53, 0x61, 0x21, 0x45 } },
  { 7,  { 0x45, 0x47, 0x49, 0x53, 0x60, 0x00, 0x00 } },
  { 7,  { 0x45, 0x47, 0x49, 0x53, 0x60, 0x01, 0x00 } },
  { 9,  { 0x45, 0x47, 0x49, 0x53, 0x63, 0x2c, 0x02, 0x00, 0x57 } },
  { 7,  { 0x45, 0x47, 0x49, 0x53, 0x60, 0x2d, 0x00 } },
  { 7,  { 0x45, 0x47, 0x49, 0x53, 0x62, 0x67, 0x03 } },
  { 7,  { 0x45, 0x47, 0x49, 0x53, 0x60, 0x0f, 0x00 } },
  { 9,  { 0x45, 0x47, 0x49, 0x53, 0x63, 0x2c, 0x02, 0x00, 0x13 } },
  /* The init sequence leaves the gain at zero, so set a working point. */
  { 7,  { 0x45, 0x47, 0x49, 0x53, 0x61, EGIS057E_REG_GAIN,
          EGIS057E_GAIN_VALUE } },
  { 7,  { 0x45, 0x47, 0x49, 0x53, 0x61, EGIS057E_REG_OFFSET,
          EGIS057E_OFFSET_VALUE } },
};

#define EGIS057E_INIT_TOTAL (G_N_ELEMENTS (egis057e_init_pkts))

/*
 * Re-arms an acquisition without a USB reset.
 *
 * Without it the sensor answers every image request with the very same buffer,
 * byte for byte: seventy-six repeats with not a single byte different. Of the
 * candidates tried, this is the only one that produces fresh frames; the
 * others leave the sensor frozen. Re-opening the device works too, but costs a
 * USB reset and about two seconds - far too slow to follow a finger.
 */
static const Egis057ePkt egis057e_rearm_pkt =
{ 9, { 0x45, 0x47, 0x49, 0x53, 0x63, 0x2c, 0x02, 0x00, 0x13 } };

/* "EGIS" 0x64 <length high> <length low> */
static const Egis057ePkt egis057e_imgreq_pkt =
{ 7, { 0x45, 0x47, 0x49, 0x53, 0x64,
       (EGIS057E_BLOCKSIZE >> 8) & 0xff, EGIS057E_BLOCKSIZE & 0xff } };

#endif
