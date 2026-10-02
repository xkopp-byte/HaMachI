# Napojenie UI na dodanú komunikáciu

Úprava umožňuje skúšanie nového okna s KobukiSim a používa pôvodné porty a URL kamery zo zadanej IP. Toto je zoznam zmien pri napájaní UI; vzhľad okna bol upravený už predtým.

## mainwindow.cpp a mainwindow.h

- `MainWindow::MainWindow()` – napojenie existujúcich UI signálov Connect, Disconnect, Stop a pohybu na komunikačný adaptér; časovač na kontrolu príjmu údajov.
- `MainWindow::~MainWindow()` – ukončenie aktívneho spojenia pred zrušením UI.
- Nová `startRobotSession(address, simulation)` – vytvorí objekt `robot`, pripojí jeho signály a zavolá pôvodnú `initAndStartRobot(address)`. Prvé robotové údaje potvrdia Connected. Kamerové snímky prevádza z OpenCV BGR/BGRA/GRAY do QImage RGB a odovzdá ich `setCameraImage()`.
- Nová `endRobotSession(message)` – vyžiada nulovú rýchlosť, ukončí vlákna, zahodí staré snímky a obnoví odpojený stav. Identifikátor relácie blokuje oneskorené callbacky starého spojenia.
- Nová `checkRobotSession()` – po 5 s bez prvých robotových údajov ukončí pokus; pri spojení kontroluje výpadok údajov dlhší než 2 s. Po 3 s bez obrazu skryje starú snímku.
- Existujúce frontendové metódy `toggleConnection()`, `setConnectionState()`, `requestMotion()`, `setCameraImage()`, `setWarning()`, `setDanger()`, `setObstacleIndicators()`, `applyTheme()` a `eventFilter()` zostávajú rozhraním UI. Pri tomto napojení sa ich telá nemenili.

## robot.cpp a robot.h

- `robot::robot()` – inicializácia existujúcich premenných, ktoré boli pôvodne neinicializované. Inicializácia x/y/fi na nulu nie je implementácia odometrie.
- Nový `robot::~robot()` a `robot::stopRobot()` – ukončia komunikačné vlákna pred zrušením dát objektu.
- `robot::setSpeedVal()` a `robot::setSpeed()` – mutex chráni povely pred súbežným prístupom UI a komunikačného callbacku. Pôvodný výber pohybových povelov ostáva.
- `robot::processThisRobot()` – rovnaký mutex a nový signál `telemetryReceived()` pri prijatých robotových údajoch.
- `robot::processThisCamera()` – odmieta prázdny snímok a posiela vlastnú kópiu obrazu namiesto zdieľaného kruhového bufferu.
- `robot::initAndStartRobot()` – pôvodná funkcia bez zmeny portov a URL.

## librobot.cpp a librobot.h

- `libRobot::~libRobot()` – používa spoločnú funkciu na ukončenie.
- Nová `libRobot::robotStop()` – signalizuje ukončenie, počká na vlákna, zavrie sockety a umožní ďalšie spustenie.
- `libRobot::robotStart()` – chráni pred opakovaným spustením aktívnych vlákien a obnoví ukončovací promise/future.
- `libRobot::robotprocess()` – preskočí neúspešné alebo prázdne UDP čítanie pred dekódovaním paketu.
- `libRobot::imageViewer()` – príjem cez OpenCV FFmpeg s timeoutmi, opakovaným pokusom po výpadku a odovzdávaním platných snímok.

## udp_communication.cpp a udp_communication.h

- `udp_communication::init_connection()` – upratanie predchádzajúceho socketu na Windows a evidencia inicializácie Winsock. Na Linuxe pridaný prijímací timeout; na Windows už timeout existoval.
- Nová `udp_communication::closeConnection()` – zavretie socketu a zodpovedajúce uvoľnenie Winsock.
- `udp_communication::~udp_communication()` – volá `closeConnection()`.
- Socket na Windows používa typ `SOCKET` a začína hodnotou `INVALID_SOCKET`.

## Zostavenie a skúšanie

V `demoRMR/CMakeLists.txt` je kopírovanie nájdeného OpenCV FFmpeg pluginu vedľa programu. Vzhľad `mainwindow.ui` sa pri tomto napojení nemenil.

Debug kompilácia a `git diff --check` prešli. Kamerový endpoint `http://127.0.0.1:8000/stream.mjpg` pri kontrole odmietol spojenie, preto zatiaľ nie je potvrdený príjem obrazu zo spusteného simulátora ani pohyb.

Spusti KobukiSim, v prípade potreby v ňom vygeneruj obraz, potom nové zostavenie demoRMR. Zadaj 127.0.0.1 a stlač Connect. Pri prijatí robotových údajov sa zobrazí Connected. V Controls drž smerové tlačidlo; pri uvoľnení sa odošle Stop. Veľký SAFETY STOP odosiela nulovú rýchlosť cez pôvodný povel, nejde o hardvérové núdzové odpojenie.

Camera aj Simulation v tomto adaptéri používajú stream zadanej IP. Výber zatiaľ nemení endpoint na druhý nezávislý zdroj. Parkovací asistent, fúzia pásikov a vyhodnotenie WARNING/DANGER nie sú doplnené.
