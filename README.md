# Программа управления 4-канальным реле  

## Общее описание
Этот проект представляет собой программу для управления 4-канальным модулем реле на базе ESP32S3. Устройство предназначено для удаленного управления реле через Wi-Fi. Программа запускает на чипе ESP32S3 http-сервер с простейшим REST API, позволяющим контролировать реле через HTTP-запросы.    
![Сам модуль реле выглядит так](https://raw.githubusercontent.com/houseofbigseals/esp32_relay/commit/image.png)   
Модуль может контролировать 4 канала реле, а также датчики температуры ds18b20 и DHT-XX. Для контроля температуры самого девайса на плате размещен датчик HDC1080.
В проекте используется реализация wireguard для esp32 от [trombik](https://github.com/trombik/esp_wireguard) и его форк [Текст ссылки](droscy://github.com/droscy/esp_wireguard)


## Сетевые интерфейсы
Устройство  доступно через следующие сетевые интерфейсы:
- Локальный IPv4 адрес - обычно назначается роутером случайно, поэтому непригоден к использованию  
- Статический IPv4 адрес в сети Wireguard. По нему может быть доступен удаленно, если клиент тоже подключен к впн-сети  
- Статический локальный адрес mDNS. Вам нужен mDNS сервис типа Avahi чтобы найти в локальной сети девайс по его имени, но это быстрее чем через впн.  

## Подключение к Wi-Fi
Пока только через перепрошивку прибора.

## Веб-интерфейс
Устройство поддерживает REST API для управления реле. Ниже приведен список доступных команд:  
 

### Получение состояния всех реле и датчиков
```
оо
```

### Получение состояния конкретного датчика
```
оо
```

### Изменение состояния реле
```
оо
```

## Как билдить код
Очень важно выставить правильные параметры в sdkconfig, иначе wireguard просто не соберется. 
Еще программа используем измененную схему разделов в памяти, так что надо подключить использование partitions.csv в разделе Flash  

