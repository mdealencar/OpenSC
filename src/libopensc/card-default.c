/*
 * card-default.c: Support for cards with no driver
 *
 * Copyright (C) 2001  Juha Yrj�l� <juha.yrjola@iki.fi>
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

#include "sc-internal.h"
#include "sc-log.h"

static struct sc_card_operations default_ops;
static const struct sc_card_driver default_drv = {
	"Default driver for unknown cards",
	"default",
	&default_ops
};

static int default_finish(struct sc_card *card)
{
	return 0;
}

static int default_match_card(struct sc_card *card)
{
	return 1;		/* always match */
}

static int autodetect_class(struct sc_card *card)
{
	int classes[] = { 0x00, 0xC0, 0xB0, 0xA0 };
	int class_count = sizeof(classes)/sizeof(int);
	u8 rbuf[SC_MAX_APDU_BUFFER_SIZE];
	struct sc_apdu apdu;
	int i, r;

	if (card->ctx->debug >= 2)
		debug(card->ctx, "autodetecting CLA byte\n");
	for (i = 0; i < class_count; i++) {
		if (card->ctx->debug >= 2)
			debug(card->ctx, "trying with 0x%02X\n", classes[i]);
		apdu.cla = classes[i];
		apdu.cse = SC_APDU_CASE_1;
		apdu.ins = 0xC0;
		apdu.p1 = apdu.p2 = 0;
		apdu.datalen = 0;
		apdu.lc = apdu.le = 0;
		r = sc_transmit_apdu(card, &apdu);
		SC_TEST_RET(card->ctx, r, "APDU transmit failed");
		if (apdu.sw1 == 0x6E)
			continue;
		if (apdu.sw1 == 0x90 && apdu.sw2 == 0x00)
			break;
		if (apdu.sw1 == 0x61)
			break;
		if (card->ctx->debug >= 2)
			debug(card->ctx, "got strange SWs: 0x%02X 0x%02X\n",
			      apdu.sw1, apdu.sw2);
		break;
	}
	if (i == class_count)
		return -1;
	card->cla = classes[i];
	if (card->ctx->debug >= 2)
		debug(card->ctx, "detected CLA byte as 0x%02X\n", card->cla);
	if (apdu.resplen < 2) {
		if (card->ctx->debug >= 2)
			debug(card->ctx, "SELECT FILE returned %d bytes\n",
			      apdu.resplen);
		return 0;
	}
	if (rbuf[0] == 0x6F) {
		if (card->ctx->debug >= 2)
			debug(card->ctx, "SELECT FILE seems to behave according to ISO 7816-4\n");
		return 0;
	}
	if (rbuf[0] == 0x00 && rbuf[1] == 0x00) {
		const struct sc_card_driver *drv;
		if (card->ctx->debug >= 2)
			debug(card->ctx, "SELECT FILE seems to return Schlumberger 'flex stuff\n");
		drv = sc_get_flex_driver();
		card->ops->select_file = drv->ops->select_file;
		return 0;
	}
	return 0;
}

static int default_init(struct sc_card *card)
{
	int r;
	
	card->drv_data = NULL;
	r = autodetect_class(card);
	if (r) {
		error(card->ctx, "unable to determine the right class byte\n");
		return SC_ERROR_INVALID_CARD;
	}

	return 0;
}

static const struct sc_card_driver * sc_get_driver(void)
{
	const struct sc_card_driver *iso_drv = sc_get_iso7816_driver();

	default_ops = *iso_drv->ops;
	default_ops.match_card = default_match_card;
	default_ops.init = default_init;
        default_ops.finish = default_finish;

        return &default_drv;
}

#if 1
const struct sc_card_driver * sc_get_default_driver(void)
{
	return sc_get_driver();
}
#endif
