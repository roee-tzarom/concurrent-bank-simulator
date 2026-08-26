FROM gcc:12

WORKDIR /app

# מעתיק את קבצי המקור אל תוך הקונטיינר
COPY bank.c Makefile accounts.txt ./

# מקמפל את התוכנית
RUN make

# הפקודה שתרוץ כברירת מחדל
CMD ["./bank"]