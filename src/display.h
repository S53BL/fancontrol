// display.h — GxEPD2 ePaper WeAct 2.9" B/W
#pragma once

void initDisplay();    // Inicializacija GxEPD2
void updateDisplay();  // Full refresh zaslona
void partialDisplay(); // Partial refresh (če podprt)
