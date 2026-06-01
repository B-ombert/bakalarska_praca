# Web stránka bakalárskej práce

Tento repozitár slúži na prehľad postupu pri tvorbe bakalárskej práce.

---

## Informácie o študentovi

- *Meno študenta:* Norbert Benko
- *Názov práce:* Open-source desktopová multiplatformová kalendárová aplikácia 
- *Meno školiteľa:* Ing. Alexander Šimko, PhD.
- *Kontakt na študenta:* benko85@uniba.sk  

---

## Zadanie práce

- *Anotácia:*  
Jeden z populárnych prístupov k tvorbe desktopových aplikácií je mať viacero špecializovaných aplikácií a nekombinovať viacero aplikácii do jednej. Na poli open-source aplikácií chýba multiplatformová desktopová aplikácia, ktorá by sa špecializovala výhradne na prácu s kalendárom.

- *Cieľ práce:*  
Cieľom práce je navrhnúť a implementovať open-source desktopovú multiplatformovú kalendárovú aplikáciu. Aplikácia má podporovať manažment viacerých kalendárov, kalendárových udalostí (jednorázových aj opakujúcich sa), pripomienky udalostí a pod. Aplikácia má byť schopná efektívne pracovať s desiatkami kalendárov obsahujúcimi rádovo milióny udalostí. Aplikácia má fungovať v offline režime. Zároveň má vedieť synchronizovať svoje dáta s kalendárovými poskytovateľmi ako CalDav, Google Calendar, ICloud a Microsoft Exchange.

---

## [Text bakalárskej práce](https://www.overleaf.com/read/pnwdxtdqdfwy#c00344)

---

## Zdroje a odkazy

- **Technické dokumentácie a špecifikácie:**
  - [RFC 6749: The OAuth 2.0 Authorization Framework](https://datatracker.ietf.org/doc/rfc6749/)
  - [RFC 7636: Proof Key for Code Exchange by OAuth Public Clients](https://datatracker.ietf.org/doc/rfc7636/)
  - [RFC 5545: Internet Calendaring and Scheduling Core Object Specification (iCalendar)](https://datatracker.ietf.org/doc/rfc5545/)
  - [Google Calendar API Documentation](https://developers.google.com/calendar/api)
  - [Microsoft Graph Calendar API](https://learn.microsoft.com/en-us/graph/api/resources/calendar)
  - [Microsoft Graph: Event Resource](https://learn.microsoft.com/en-us/graph/api/resources/event)

- **Použité knižnice a nástroje:**
  - [wxWidgets Documentation](https://wxwidgets.org/docs/)
  - [Boost.Asio Documentation](https://www.boost.org/libs/asio)
  - [Boost.Beast Documentation](https://www.boost.org/library/latest/beast/)
  - [SQLite Documentation](https://www.sqlite.org/docs.html)


---

## Denník

- **1. týždeň**
  - Rozšírenie pôvodného skriptu na prihlasovanie cez OAuth.
  - Doplnené získavanie a ukladanie refresh tokenu, aby aplikácia nemusela pri každom spustení vyžadovať nové manuálne prihlásenie.

- **2. týždeň**
  - Návrh základného dátového modelu aplikácie.
  - Pribudli entity pre účty, kalendáre a udalosti, aby aplikácia nemusela pracovať iba s jedným hlavným kalendárom a jednou hardcoded udalosťou.

- **3. týždeň**
  - Implementácia lokálnej SQLite databázy.
  - Začalo sa ukladať viacero účtov, kalendárov a udalostí lokálne, čo vytvorilo základ pre offline-first fungovanie aplikácie.

- **4. týždeň**
  - Zavedenie repository vrstvy nad databázou.
  - Databázové operácie sa oddelili od zvyšku logiky aplikácie, čím sa zjednodušila práca s účtami, kalendármi a udalosťami.

- **5. týždeň**
  - Rozšírenie sťahovania dát zo servera.
  - Namiesto downloadu udalostí iba z hlavného kalendára sa začal získavať zoznam kalendárov používateľa a udalosti z jednotlivých kalendárov.

- **6. týždeň**
  - Implementácia synchronizačných stavov udalostí.
  - Lokálne vytvorené, upravené a zmazané udalosti sa začali označovať stavmi ako pending insert, pending update a pending delete.
  
- **7. týždeň**
  - Rozšírenie uploadu udalostí.
  - Namiesto odosielania jednej hardcoded udalosti začala aplikácia odosielať reálne lokálne zmeny z databázy na vzdialený server.

- **8. týždeň**
  - Doplnenie podpory pre viacero poskytovateľov.
  - Synchronizačná logika sa začala deliť na spoločnú časť a špecifické časti pre Google Calendar a Microsoft Outlook Calendar.

- **9. týždeň**
  - Pridanie práce s access tokenmi v rámci synchronizácie.
  - Token sa začal získavať cez spoločnú triedu, ktorá kontroluje expiráciu a obnovuje access token pomocou refresh tokenu.

- **10. týždeň**
  - Implementácia asynchrónneho spracovania sieťových operácií.
  - Prihlasovanie, sťahovanie dát a upload lokálnych zmien sa presunuli mimo hlavného vykonávacieho threadu, aby neskôr neblokovali používateľské rozhranie.
 
- **11. týždeň**
  - Začiatok implementácie desktopového používateľského rozhrania.
  - Pribudlo hlavné okno aplikácie, základné zobrazenie kalendára a prvé prepojenie UI s lokálnou databázou.

- **12. týždeň**
  - Rozšírenie používateľského rozhrania o prácu s udalosťami a kalendármi.
  - Používateľ mohol vytvárať, upravovať a mazať udalosti, prepínať zobrazené obdobia a pracovať s viacerými kalendármi.

- **13. týždeň**
  - Doplnenie pokročilejších funkcií aplikácie.
  - Pribudla správa účtov, práca s read-only kalendármi, opakované udalosti, časové pásma, celodenné udalosti a optimalizácia synchronizácie iba pre potrebné časové obdobia.


