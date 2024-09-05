# Proxy
In this project, I implemented a concurrent web proxy that logs requests. A web proxy acts as a middleman between a web browser and an end server. Instead of contacting the end server directly, the browser communicates with the proxy, which forwards the request to the server. When the server responds, the proxy relays the reply back to the browser.

Proxies serve various purposes: they can be used in firewalls to regulate web access, strip requests of identifying information to anonymize browsing, and even cache web pages to improve load times. They can also perform transformations, such as removing ads from web pages.

In the first part of this project, I developed a basic sequential proxy that handles requests one at a time, forwarding them to the server and returning the results to the browser while logging the requests to a disk file. This gave me hands-on experience with network programming and the HTTP protocol.

In the second part, I upgraded the proxy to handle multiple clients concurrently using threads, allowing it to manage multiple requests efficiently. This enhancement deepened my understanding of concurrency and multithreading in network applications.
