"Collection — abstract superclass for all collections.
 Iteration methods are subclassResponsibility; concrete subclasses
 (SequenceableCollection, etc.) provide implementations."

!Collection methodsFor: 'enumerating'!
do: aBlock
    self subclassResponsibility
!

collect: aBlock
    self subclassResponsibility
!

select: aBlock
    self subclassResponsibility
!

reject: aBlock
    self subclassResponsibility
!

detect: aBlock ifNone: noneBlock
    self subclassResponsibility
! !

!Collection methodsFor: 'testing'!
isEmpty
    ^ self size = 0
!

notEmpty
    ^ self isEmpty not
! !

!Collection methodsFor: 'accessing'!
size
    self subclassResponsibility
! !
