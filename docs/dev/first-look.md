# Первый просмотр VaRest 2.0

Обычная Shipping-игра теперь показала и публичный WebSocket: реальный echo, локальный Close1000, независимый журнал сетевого закрытия и штатный выход0 за13.793с. Первый из16 настоящих кадров галереи — новая карта007; HTTP200→201 сохранён следующим кадром. Engine, Host, Target и игровой код не менялись; исправлено только ошибочное ожидание новой демосцены от локального callback.

Текущий проект использует неизменённый source engine и обычные модули плагина. Штатные Editor, UnrealGame Development и Shipping собраны; обычная потребляющая Shipping-игра HostProject прошла все949 действий сборки, стандартную упаковку BuildCookRun PAK/IoStore и настоящий запуск. Ручной игровой модуль, `Target.cs` или правки Engine не нужны: временные целевые файлы проекта создаёт сам SDK при включении плагина. OpenAPI-проверки11/0 прошли; отдельная регрессия1/0 вызывает настоящий Save-валидатор до и после повторного импорта.

Northwind из [документа съёмки](../examples/first-look/NorthwindGameApi.json) уже импортирован в `/Game/FirstLook/DA_NorthwindGameApi`: API, четыре структуры и два enum сохранены на диск. Проверка `-Check` в новом процессе загрузила их и вернула отсутствие изменений. В отдельном настоящем Editor ассет открыт и сохранён кнопкой Save: ошибок типов нет, остаются предупреждения о добавлении API в Settings.Apis. Окно ассета и Editor закрылись штатно, код выхода 0. Это проверено с обычным параметром движка `-norhithread`; прежний сбой стандартного Vulkan/RHI-режима остаётся открытым. Ранее повреждённый демо-ассет сохранён отдельно и автоматически не мигрируется.

[Шестнадцать настоящих скриншотов в одном офлайн HTML](../demo/unreal-editor-gallery.html). Первый — публичный WebSocket в обычном cooked Shipping; следующий — реальный типизированный GET200 → POST201. Все исходные PNG сохранены без обработки. Старый ошибочный006 кадр не выдаётся за успешный.

Northwind/Save и девять исторических интерфейсов Editor остаются в той же галерее; прежний Development-кадр с PrintString сохранён отдельно. Это разные фактические проверки, а не все интерфейсы в Shipping.

Для просмотра без Unreal откройте [интерактивный HTML-макет](../demo/varest2-interface-preview.html): восемь экранов с демоданными, локальной имитацией запросов и сообщений. Это отдельная иллюстрация интерфейсов.

## Открыть собранный проект

На машине с графическим сеансом запустите:

```bash
/w/Unreal/UnrealEngine/Engine/Binaries/Linux/UnrealEditor \
  /w/Unreal/VaRest-recovery/takeover-01441-20261107/stock-plugin-editor-graph-002/HostProject/HostProject.uproject \
  -norhithread
```

Проект уже включает плагин VaRest и сохранённый `/Game/FirstLook/DA_NorthwindGameApi`. `-norhithread` — штатный параметр неизменённого Engine, использованный при успешной проверке на этой машине после сбоя NVIDIA/Vulkan в другом режиме; это не требование к поставке плагина. Не используйте `-unattended` или `-nullrhi` для просмотра панели Inspector. Если Editor предлагает пересобрать модули, не соглашайтесь автоматически: сохраните сообщение, это расхождение с зафиксированной сборкой. Перед следующей сборкой модулей плагина Editor должен быть закрыт.

## Публичный WebSocket в обычной Shipping-игре

В Editor доступны обычные `/Game/VaRestFirstLookSocket007/FirstLook` и `BP_PublicSocket`: компонент `VaRestSocketComponent`, публичный клиент, привязки событий, Connect, SendText и Close. Ручная карта остаётся открытой; `FirstLookAuto` завершает игру через10с после результата. Новые UCLASS, игровой модуль или Target для сцены не нужны.

В первом терминале запустите **одноразовый** локальный peer с новым именем журнала:

```bash
python3 /w/Unreal/VaRest/docs/examples/first-look/demo-websocket-server.py \
  --receipts ./varest-public-socket-demo.jsonl
```

После сообщения `PUBLIC_SOCKET_DEMO_READY` во втором терминале с графическим сеансом:

```bash
/w/Unreal/VaRest-recovery/takeover-01441-20261107/public-cooked-ws-blueprint-local-close-package-001/archive/Linux/HostProject/Binaries/Linux/HostProject-Linux-Shipping \
  -game -norhithread -windowed -ResX=1440 -ResY=900
```

Ожидаются `Echo matched; local Close requested`, одинаковые Requested/Echo со строкой `"VaRest cooked WebSocket demo"` и `Local Close:1000`. По контракту публичный Close сразу сообщает локальное отключение; эта подпись не утверждает, что callback получил окончательный ACK сервера. Независимый peer проверяет настоящий masked text, echo, оба Close1000 и EOF, после чего сам заканчивает работу. Для каждого нового запуска перезапустите peer и задайте свободное имя журнала; чужие заголовки/тела он не сохраняет.

Автоматически выполнен stage-вариант этого настоящего PAK/IoStore пакета: Game0, peer0, нефорсированный выход13.793с, все79458 проверок сохранности. Все22 пары stage/archive одинаковы. Реальная карта выбирается обычным GameDefaultMap, включённым при UAT007; исходный отдельный конфиг демопроекта после упаковки восстановлен на HTTP005. Shipping карту из CLI не принимает. Пример использует WS/localhost/IPv4-loopback, не подтверждает публичный WSS/CA/proxy или весь lifecycle/release набор.

## Подготовленная сцена и настоящая Shipping-игра

В Editor откройте `/Game/VaRestFirstLookShipping005/FirstLook` и `BP_NorthwindVisual` из той же папки. Это обычные Blueprint и уровень. BeginPlay вызывает GetPlayerProfile, затем поля `displayName/level/online` идут в типизированное тело CreateMatch. Ответы и ошибки выводятся в реальные текстовые компоненты. Ручная карта остаётся открытой; отдельная `FirstLookAuto` завершается через10с после ответа, без задержки перед запросами.

В первом терминале запустите входящий в репозиторий локальный сервер. Он принимает только фиксированные публичные демоданные, не пишет произвольные тела/заголовки/секреты. Имя журнала должно быть свободно:

```bash
python3 /w/Unreal/VaRest/docs/examples/first-look/demo-typed-http-server.py \
  --receipts ./varest-typed-first-look-http.jsonl
```

Во втором терминале на машине с графическим сеансом:

```bash
/w/Unreal/VaRest-recovery/takeover-01441-20261107/private-lws-async-dns-normal-package-001/archive/Linux/HostProject/Binaries/Linux/HostProject-Linux-Shipping \
  -game -norhithread -windowed -ResX=1440 -ResY=900
```

Ожидается экран из HTTP-кадра галереи: Profile Alex Demo, HTTP profile200, Match ID, Players27, HTTP match201. Игра сама закроется примерно через10с после ответа; сервер останавливается Ctrl+C. В автоматической проверке запускалась stage-копия этого пакета с настоящими PAK/IoStore; все22 пары файлов stage/archive побайтно одинаковы. Использовались изолированный loopback, настоящий Shipping ELF и исходные пиксели окна. Код обработчиков сервера в репозитории совпадает с использованным сервером; отдельно изменён только текст docstring.

Shipping штатно игнорирует карту из командной строки. Нужная `GameDefaultMap=/Game/VaRestFirstLookShipping005/FirstLookAuto` задана обычным новым `Config/Linux/LinuxEngine.ini` **только в отдельном демопроекте** и вошла в пакет при штатной упаковке. Существующий SDK DefaultEngine.ini, Engine config, Target и исходники плагина при этом не менялись. Для длительного ручного просмотра используйте карту FirstLook в Editor.

Это результат обычного BuildCookRun: Cook, Stage, PAK/IoStore, Package и Archive завершились0 за62,005с, все79034 входа сохранились. Настоящий Game receipt получен сборкой949, временные целевые файлы созданы самим UnrealBuildTool. Нет fake receipt/readiness/installed/precompiled файлов. Прежний diagnostic loose stage сохранён как отдельный проверенный результат. Эта конечная игровая проверка не закрывает defaultRHI, WS/DNS/lifetime, остальные платформы или полный релизный набор.

VaRest включается явно через Plugins UI либо `VaRest: Enabled=true` в `.uproject`; демопроект уже содержит такую запись. Копирование папки само по себе больше не активирует плагин. Development-сцена002 и её успешная отдельная игровая регрессия1/0 сохранены как предыдущая проверка.

## Посмотреть API-ассет и импорт

1. В Content Browser создайте папку `/Game/VaRestFirstLook`.
2. Через **Import** выберите `docs/examples/first-look/DemoApi.json` из репозитория. Это небольшой OpenAPI3 JSON без авторизации и внешних ссылок. В preview проверьте операцию `GetDemoPlayer`, путь `/player.json` и тип `DemoPlayer`, затем примените импорт.
3. Откройте созданный **VaRest API**-ассет: посмотрите окружение с Base URL `http://127.0.0.1:8080`, endpoint и сгенерированный тип ответа. Импорт помечает пакеты dirty; сохранение делается вами явно.
4. Для ручного создания пустого ассета используйте категорию **VaRest → VaRest API** в меню создания Content Browser. У импортированного API контекстное меню **VaRest** содержит действия reimport/check/report.

Для первого запуска с правилами добавьте API в массив **Apis** в Project Settings → Plugins → **VaRest HTTP**. Это настройка вашего тестового проекта; инструкция сама конфигурацию не изменяет.

## Увидеть локальный HTTP-запрос

Необязательный стандартный файловый сервер отдаёт только эту папку примера. Запустите его сами в отдельном терминале:

```bash
python3 -m http.server 8080 --bind 127.0.0.1 \
  --directory /w/Unreal/VaRest/docs/examples/first-look
```

`http://127.0.0.1:8080/player.json` должен вернуть `id=42`, `displayName="VaRest demo"`, `online=true`. Сервер не содержит логики приложения; остановка — Ctrl+C. Именно этот файловый сервер/PIE-сценарий агентом не запускался. Для снимков использовался отдельный собственный сервер на18423 и настоящие async HTTP actions из Editor. Если8080 занят, выберите другой порт и измените Base URL в тестовом API.

В пустом уровне откройте Level Blueprint, добавьте **Call Endpoint**, выберите в Details импортированный API и соответствующий `GetDemoPlayer` endpoint. Подключите BeginPlay к выполнению запроса, успешный и ошибочный выходы — к своим Print String. Посмотрите сгенерированный пин ответа и его поля через Break Struct. Скомпилируйте Blueprint и запустите PIE. Реальные названия endpoint-ноды меняются после выбора операции; её начальное название — Call Endpoint.

## Inspector

Откройте **Tools → VaRest Network Inspector** (секция Debug) либо выполните в консоли Editor:

```text
VaRest.Inspector.Open
VaRest.Inspector.Capture 1
```

После выполнения запроса выберите соответствующий PIE/world scope либо все scopes. Ожидаемый результат локального примера — GET `/player.json`, HTTP200 и JSON из файла. Наличие body зависит от Capture Bodies в настройках Inspector; включайте его только для несекретных демоданных. В этой инструкции нет токенов, аккаунтов или внешнего сервиса.

Этот раздел — маршрут для самостоятельной проверки; точный выполненный объём отдельного просмотра указан в начале и в галерее. Если команда открытия, импорт, компиляция Blueprint или запрос дают ошибку, сохраните Output Log и этап воспроизведения.

## Что пока не входит в этот просмотр

Host5 с правками движка отменён. Публичный cooked WS/echo/Close теперь проверен в конечном примере выше. Общие DNS/provider/lifetime, полный Socket.IO interoperability, worker/cooked-reflection, игровые/платформенные и релизные проверки остаются отдельной незавершённой работой. Для просмотра JSON/HTTP/OpenAPI/Inspector в обычном Editor включать native-owner флаг не нужно. Прогресс и границы подтверждённых запусков: [development passport](../passport/2026-11-04-varest2-development.md).
