# Первый просмотр VaRest 2.0

Строгий стандартный Linux BuildPlugin для Source377 теперь завершился0 во всех трёх стадиях: Editor493, Game Development320 и Shipping316. Проверены1891файл сырого пакета/841исходник/853продукта манифестов, все142555входов сохранены;1114свежих .d join4287входов без неподтверждённых зависимостей. Это проверка производства пакета, не запуск в новой игре: обычный проект-потребитель и новая проверка runtime ещё впереди. README/LICENSE не вошли в сырой SDKфильтр и требуют чистой сборки комплекта поставки. Ниже сохранены реальные кадры и runtime code373 своего прежнего интервала; после новой сборки SDK старый проект нельзя считать автоматически перепроверенным.

Последняя проверенная сборка плагина code373 проверен в обычной упакованной Shipping-игре без смены режима RHI: официальный Socket.IO4.8.4 вернул типизированный ACK73, игра и peer завершились0 без принудительной остановки за13.210с. Все108450 защищённых входов сохранены. Engine, Host, Target и игровой код не модифицировались; сцена использует обычный Blueprint и публичный SioComponent. Предыдущие HTTP/WS/Socket.IO кадры сохраняют свою собственную историю.

Штатные Editor и Game Development/Shipping собраны из обычного GitHub Engine. Текущая обычная suite Editor прошла964/964; Game Development —21/0 со штатным SoftQuit. Совпадающая проектная Shipping-сборка прошла33 действия0 (28CPP плагина, стандартные PCH, SDK-generated FDefaultModuleImpl, Link/receipt), без компиляции Engine CPP. Ручной игровой модуль или Target.cs не нужен: временные файлы создаёт SDK. Новый стандартный UAT Cook/Stage/PAK-IoStore/Package/Archive завершился0; все22 пары stage/archive идентичны.

Northwind из [документа съёмки](../examples/first-look/NorthwindGameApi.json) уже импортирован в `/Game/FirstLook/DA_NorthwindGameApi`: API, четыре структуры и два enum сохранены на диск. Проверка `-Check` в новом процессе загрузила их и вернула отсутствие изменений. В отдельном настоящем Editor ассет открыт и сохранён кнопкой Save: ошибок типов нет, остаются предупреждения о добавлении API в Settings.Apis. Окно ассета и Editor закрылись штатно, код выхода 0. Это проверено с обычным параметром движка `-norhithread`; позднее отдельное открытие текущего API в обычном режиме Vulkan и выход0 подтверждены ниже, без универсальной гарантии для всех прежних сбоев. Ранее повреждённый демо-ассет сохранён отдельно и автоматически не мигрируется.

[Девятнадцать настоящих скриншотов в одном офлайн HTML](../demo/unreal-editor-gallery.html#current-shipping-default-rhi). Кадр19 показывает текущую Shipping/code373 без -norhithread. Первые18 сохранены с исходными PNG и историческими условиями; ранние игровые HTTP/WS/Socket.IO проверки использовали -norhithread. Старый ошибочный006 кадр не выдаётся за успешный.

Northwind/Save и девять исторических интерфейсов Editor остаются в той же галерее; прежний Development-кадр с PrintString сохранён отдельно. Это разные фактические проверки, а не все интерфейсы в Shipping.

Для просмотра без Unreal откройте [интерактивный HTML-макет](../demo/varest2-interface-preview.html): восемь экранов с демоданными, локальной имитацией запросов и сообщений. Это отдельная иллюстрация интерфейсов.

## Текущий Editor без смены режима RHI

В новом обычном проекте текущий API-ассет Northwind открыт в настоящем окне VaRest. Главное окно закрыто через File → Exit при всё ещё открытом API-редакторе: **выход0 без принудительной остановки**,343.666с,108183 защищённых файла сохранены. `-norhithread`, Python-скрипт и demo-сервер не использовались; работали штатные плагины Engine и обычный Vulkan. [Новый исходный кадр](../demo/unreal-editor-gallery.html#current-default-rhi-api) добавлен18-м, предыдущие17 не изменены.

На этой машине можно открыть именно этот проект с семью сохранёнными Northwind-ассетами:

```bash
/w/Unreal/UnrealEngine/Engine/Binaries/Linux/UnrealEditor \
  /home/ufna/VaRest-recovery/takeover-01441-20261108-sio/current-plugin-asset-default-rhi-control-002/PluginAssetControl/PluginAssetControl.uproject
```

В Content Browser откройте `/Game/FirstLook/DA_NorthwindGameApi`. Движок и игровые исходники не правились; обычный дескриптор проекта подключает уже собранный VaRest. Это проверка конкретного текущего окна, а не исправление всех старых сбоев NVIDIA/Editor: исходная история другого DA_DemoApi и режим packaged Game остаются отдельными. Не отключайте все стандартные engine-плагины для такой проверки: эксперимент с `DisableEnginePluginsByDefault` исключил штатный PropertyAccessEditor, нужный системам Animation/UMG, и остановился ещё до открытия API.

## Предыдущий проект и сохранённый обходной режим

На машине с графическим сеансом запустите:

```bash
/w/Unreal/UnrealEngine/Engine/Binaries/Linux/UnrealEditor \
  /w/Unreal/VaRest-recovery/takeover-01441-20261107/stock-plugin-editor-graph-002/HostProject/HostProject.uproject \
  -norhithread
```

Проект уже включает плагин VaRest и сохранённый `/Game/FirstLook/DA_NorthwindGameApi`. `-norhithread` — штатный параметр неизменённого Engine, использованный при успешной проверке на этой машине после сбоя NVIDIA/Vulkan в другом режиме; это не требование к поставке плагина. Не используйте `-unattended` или `-nullrhi` для просмотра панели Inspector. Если Editor предлагает пересобрать модули, не соглашайтесь автоматически: сохраните сообщение, это расхождение с зафиксированной сборкой. Перед следующей сборкой модулей плагина Editor должен быть закрыт.

## Публичный Socket.IO с типизированным ACK в Shipping

В отдельном обычном проекте `/home/ufna/VaRest-recovery/takeover-01441-20261108-sio/stock-plugin-editor-interop-001/VaRestInterop/VaRestInterop.uproject` доступны `/Game/VaRestFirstLookSio002/FirstLook` и `BP_PublicSio`: обычный Actor, SioComponent и публичный Emit With Ack. Карта FirstLook остаётся открытой; FirstLookAuto завершает игру через10с после результата. Создание и холодное открытие реальных файлов прошли по1/0; ниже проверен настоящий упакованный Game. Ручной Host, Target или игровой C++ для сцены не нужны.

В первом терминале запустите строгий peer с **новым свободным именем журнала**; порты19700–19702 должны быть свободны. Закреплённые модули уже установлены для выполненной проверки:

```bash
node --no-global-search-paths /w/Unreal/VaRest/Tools/testserver/sio-interop/server.mjs \
  --server 4.8 --demo-int73 1 --port 19700 \
  --receipts ./varest-public-sio-int73-demo.jsonl \
  --modules-root /w/Unreal/VaRest-recovery/takeover-01441-20261107/socketio-reference-closed-modules-001/project
```

После `VAREST_SIO_REFERENCE_READY` во втором терминале с графическим сеансом:

```bash
cd /home/ufna/VaRest-recovery/takeover-01441-20261108-sio/current-plugin-public-sio-package-373-001/archive/Linux
./VaRestInterop/Binaries/Linux/VaRestInterop-Linux-Shipping \
  -windowed -ResX=1440 -ResY=900
```

Ожидаются реальный Sid, Namespace `/`, `Typed ACK: 73`, `Typed ACK 73 matched; local Close requested` и `Local Close:1000`. Игра закрывается сама; сервер остановите Ctrl+C. `--demo-int73 1` обязателен для этого примера: обычный режим peer принимает строку другого конечного сценария. Сервер подтверждает фактически принятое число, не подставляет готовый ответ. Для повторения задайте свободное имя журнала.

Реальный текущий прогон Source375/code373: Game0/peer0/13.210с без принудительного завершения, все108450 защищённых входа совпали; [исходный кадр19 и протокол](../demo/unreal-shipping-socketio-current-default-rhi-capture-manifest.json). Обычный режим RHI, без -norhithread/-nullrhi/driver override. Прежний прогон003/13.852с сохранён как отдельная история. Это официальный4.8.4 поверх локального WS; local Close — немедленное логическое уведомление с настоящими полями, не доказательство финального сетевого ACK. Отдельная Node-матрица10/0 покрыла4.8.4/4.5.4/4.4.1 и точный отказ несовместимой2.5.1. WSS/CA/proxy и весь lifecycle этим примером не закрыты.

Текущий обычный UAT Cook/Stage/PAK-IoStore/Package/Archive373-001 завершился0, все22 пары stage/archive идентичны. Для упаковки только в собственном демопроекте временно выбрана карта002 через LinuxEngine.ini; затем исходная карта001 восстановлена. Сам пакет уже содержит GameDefaultMap002; Shipping игнорирует выбор карты из CLI. Это данные примера, не требование установки плагина. Упаковка использует настоящие текущие Game/code373 и Editor/code373; архивный executable d50a6dbc совпадает с реальной проектной сборкой33/0 и BuildID6009f37b. Сами сохранённые Sio002 ассеты созданы и cold-проверены ранее Editor365, их байты не изменялись. UAT не компилировал C++.

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

Host5 с правками движка отменён. Публичный cooked WS/echo/Close теперь проверен в конечном примере выше. Публичные HTTP/WS/Socket.IO и конечная официальная Socket.IO-матрица подтверждены в указанных пределах. Общие DNS/provider/lifetime, fork/module-unload/global-exit, универсальная GPU/driver-квалификация, worker/cooked-reflection, остальные игровые/платформенные и релизные проверки остаются отдельной незавершённой работой. Для просмотра JSON/HTTP/OpenAPI/Inspector в обычном Editor включать native-owner флаг не нужно. Прогресс и границы подтверждённых запусков: [development passport](../passport/2026-11-04-varest2-development.md).
