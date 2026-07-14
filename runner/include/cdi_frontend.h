/* SDL presentation/input shell. It consumes published MCD212 frames and feeds
 * the transport-neutral IKAT host-input mask; it never touches guest state. */
#pragma once

int  cdi_frontend_init(void);
int  cdi_frontend_pump(void); /* zero after the player requests shutdown */
void cdi_frontend_shutdown(void);
